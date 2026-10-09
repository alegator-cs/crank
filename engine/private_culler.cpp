#include "private_culler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <deque>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

namespace crank::detail {
namespace {

// Optional compile work is capped. Query buffers are O(AST + distinct words),
// independent of the number of occurrences in the input.
constexpr std::size_t max_nodes = 65'536;
constexpr std::size_t max_edges = 262'144;
constexpr std::size_t max_compile_steps = 16'000'000;
constexpr std::size_t max_literal_bytes = 262'144;
constexpr std::size_t max_literal_size = 65'536;
constexpr std::size_t max_literals = 4'096;
constexpr std::size_t poll_interval = 64;

struct CompileLimit {};
struct QueryLimit {};
struct Cancelled {};

bool plain(const Ast& ast) {
    if (ast.has_references || ast.has_counts || ast.has_extended || ast.has_occurrence_paths)
        return false;
    // Defensive for manually constructed ASTs whose summary flags are stale.
    for (const auto& node : ast.nodes)
        if (node.kind == Kind::Reference || node.bind_count != no_id ||
            node.use_count != no_id || node.count_path != no_id)
            return false;
    return true;
}

unsigned char byte_at(std::string_view text, std::size_t position) {
    return static_cast<unsigned char>(text[position]);
}

class Work {
public:
    Work(const std::atomic_bool& cancel, const AtomicExclusions& exclusions,
         PrivateCuller::Limits limits, PrivateCuller::Stats& stats)
        : cancel_(cancel), exclusions_(exclusions), limits_(limits), stats_(stats) {
        poll();
    }
    void step() {
        if (stats_.steps >= limits_.max_steps) throw QueryLimit{};
        ++stats_.steps;
        if (++since_poll_ == poll_interval) { since_poll_ = 0; poll(); }
    }
    void poll() const {
        if (cancel_.load(std::memory_order_relaxed) || exclusions_.root_rejected()) {
            stats_.cancellation_observed_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            throw Cancelled{};
        }
    }
private:
    const std::atomic_bool& cancel_;
    const AtomicExclusions& exclusions_;
    PrivateCuller::Limits limits_;
    PrivateCuller::Stats& stats_;
    std::size_t since_poll_ = 0;
};

} // namespace

struct PrivateCuller::Compiled {
    const Ast* ast;
    const OccurrenceAnalysis* analysis;
    bool complete = false;
    bool byte_rejections = false;
    bool node_exclusions = false;
    std::bitset<256> alphabet;
    std::array<std::bitset<256>, 256> adjacent;
    std::vector<unsigned char> reachable;
    std::vector<std::vector<NodeId>> parents;
    std::vector<std::size_t> fixed_length;
    std::vector<std::size_t> literal;
    std::vector<std::string> words;

    struct State {
        std::vector<std::pair<unsigned char, std::size_t>> edges;
        std::size_t failure = 0;
        std::size_t terminal = no_id;
        std::size_t output = no_id;
    };
    std::vector<State> machine;
    std::vector<std::size_t> next_word;

    Compiled(const Ast& source, const OccurrenceAnalysis& summaries)
        : ast(&source), analysis(&summaries) {}

    std::size_t transition(std::size_t state, unsigned char byte) const {
        const auto& edges = machine[state].edges;
        const auto found = std::lower_bound(edges.begin(), edges.end(), byte,
            [](const auto& edge, unsigned char key) { return edge.first < key; });
        return found != edges.end() && found->first == byte ? found->second : no_id;
    }

    void compile() {
        if (ast->nodes.size() > max_nodes || ast->root >= ast->nodes.size() || !plain(*ast))
            return;
        std::size_t work = 0;
        const auto charge = [&](std::size_t n = 1) {
            if (n > max_compile_steps - work) throw CompileLimit{};
            work += n;
        };
        const auto count = ast->nodes.size();
        reachable.assign(count, false);
        parents.resize(count);
        fixed_length.assign(count, no_id);
        literal.assign(count, no_id);
        std::vector<NodeId> pending{ast->root};
        std::size_t edge_count = 0;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            charge();
            if (id >= count) throw CompileLimit{};
            if (reachable[id]) continue;
            reachable[id] = true;
            const auto& node = ast->nodes[id];
            // Definitions themselves are epsilon. Their bodies become reachable
            // only through a call, never merely because the definition exists.
            const auto visit = [&](NodeId child) {
                if (child >= count || ++edge_count > max_edges) throw CompileLimit{};
                parents[child].push_back(id);
                pending.push_back(child);
            };
            if (node.kind == Kind::Call) visit(node.call_target);
            else if (node.kind != Kind::Define &&
                     !(node.kind == Kind::Repeat && node.upper && *node.upper == 0))
                for (const auto child : node.children) visit(child);
        }

        const auto cross = [&](const std::bitset<256>& left,
                               const std::bitset<256>& right) {
            charge(256);
            if (right.none()) return;
            for (std::size_t byte = 0; byte != 256; ++byte)
                if (left[byte]) adjacent[byte] |= right;
        };
        for (NodeId id = 0; id != count; ++id) {
            charge();
            if (!reachable[id] || !analysis->length(id).productive) continue;
            const auto& node = ast->nodes[id];
            if (node.kind == Kind::CharSet) alphabet |= node.characters;
            if (node.kind == Kind::Reference) {
                // Defensive overapproximation for manually assembled ASTs.
                alphabet.set();
                for (auto& row : adjacent) row.set();
            } else if (node.kind == Kind::Concat) {
                std::bitset<256> prefix_last;
                for (const auto child : node.children) {
                    const auto& boundary = analysis->boundary(child);
                    cross(prefix_last, boundary.first);
                    prefix_last = boundary.nullable
                        ? prefix_last | boundary.last : boundary.last;
                }
            } else if (node.kind == Kind::Repeat && !node.children.empty() &&
                       (!node.upper || *node.upper >= 2)) {
                const auto& body = analysis->boundary(node.children.front());
                cross(body.last, body.first);
            }
        }

        // Fixed-string discovery is structural; calls deliberately remain
        // unknown. Compute lengths before constructing strings, so nested
        // concatenations do not allocate all quadratic prefix copies.
        std::vector<unsigned char> color(count, 0);
        std::vector<std::pair<NodeId, bool>> stack;
        for (NodeId origin = 0; origin != count; ++origin) {
            if (!reachable[origin] || color[origin]) continue;
            stack.emplace_back(origin, false);
            while (!stack.empty()) {
                const auto [id, finish] = stack.back(); stack.pop_back();
                charge();
                const auto& node = ast->nodes[id];
                if (!finish) {
                    if (color[id] == 2) continue;
                    if (color[id] == 1) throw CompileLimit{};
                    color[id] = 1;
                    stack.emplace_back(id, true);
                    if (node.kind != Kind::Call && node.kind != Kind::Define &&
                        !(node.kind == Kind::Repeat && node.upper && *node.upper == 0))
                        for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
                            if (reachable[*child] && color[*child] != 2)
                                stack.emplace_back(*child, false);
                    continue;
                }
                color[id] = 2;
                auto& width = fixed_length[id];
                switch (node.kind) {
                case Kind::Empty: case Kind::Define: width = 0; break;
                case Kind::CharSet:
                    if (node.characters.count() == 1) width = 1;
                    break;
                case Kind::Capture:
                    if (node.children.size() == 1) width = fixed_length[node.children.front()];
                    break;
                case Kind::Concat: {
                    std::size_t sum = 0;
                    bool known = true;
                    for (const auto child : node.children) {
                        charge();
                        const auto add = fixed_length[child];
                        if (add == no_id || add > max_literal_size - sum) { known = false; break; }
                        sum += add;
                    }
                    if (known) width = sum;
                    break;
                }
                case Kind::Alt:
                    if (node.children.size() == 1) width = fixed_length[node.children.front()];
                    break;
                case Kind::Repeat:
                    if (node.upper && *node.upper == 0 && node.lower == 0) width = 0;
                    else if (node.children.size() == 1 && node.upper && *node.upper == node.lower) {
                        const auto body = fixed_length[node.children.front()];
                        if (body != no_id && (body == 0 || node.lower <= max_literal_size / body))
                            width = node.lower * body;
                    }
                    break;
                default: break;
                }
            }
        }

        std::vector<unsigned char> covered(count, false);
        for (NodeId id = 0; id != count; ++id) {
            if (!reachable[id] || fixed_length[id] == no_id || fixed_length[id] == 0) continue;
            for (const auto child : ast->nodes[id].children) covered[child] = true;
        }
        std::unordered_map<std::string, std::size_t> interned;
        std::size_t word_bytes = 0;
        for (NodeId id = 0; id != count; ++id) {
            if (!reachable[id] || covered[id] || fixed_length[id] == no_id || fixed_length[id] == 0)
                continue;
            if (words.size() >= max_literals || fixed_length[id] > max_literal_bytes - word_bytes)
                throw CompileLimit{};
            std::string word;
            word.reserve(fixed_length[id]);
            std::vector<NodeId> todo{id};
            while (!todo.empty()) {
                charge();
                const auto part = todo.back(); todo.pop_back();
                const auto& node = ast->nodes[part];
                if (fixed_length[part] == 0) continue;
                if (node.kind == Kind::CharSet) {
                    charge(256);
                    for (std::size_t byte = 0; byte != 256; ++byte)
                        if (node.characters[byte]) { word.push_back(static_cast<char>(byte)); break; }
                } else if (node.kind == Kind::Repeat) {
                    if (node.lower > max_literal_size) throw CompileLimit{};
                    for (std::size_t n = 0; n != node.lower; ++n) todo.push_back(node.children.front());
                } else {
                    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
                        todo.push_back(*child);
                }
            }
            const auto found = interned.find(word);
            if (found != interned.end()) literal[id] = found->second;
            else {
                literal[id] = words.size();
                word_bytes += word.size();
                interned.emplace(word, words.size());
                words.push_back(std::move(word));
            }
        }

        machine.emplace_back();
        for (std::size_t id = 0; id != words.size(); ++id) {
            std::size_t state = 0;
            for (const unsigned char byte : words[id]) {
                charge(256);
                auto child = transition(state, byte);
                if (child == no_id) {
                    child = machine.size();
                    machine.emplace_back();
                    auto& edges = machine[state].edges;
                    const auto at = std::lower_bound(edges.begin(), edges.end(), byte,
                        [](const auto& edge, unsigned char key) { return edge.first < key; });
                    edges.insert(at, {byte, child});
                }
                state = child;
            }
            machine[state].terminal = id;
        }
        std::deque<std::size_t> queue;
        for (const auto& edge : machine[0].edges) queue.push_back(edge.second);
        while (!queue.empty()) {
            const auto state = queue.front(); queue.pop_front();
            charge();
            for (const auto& [byte, child] : machine[state].edges) {
                auto failure = machine[state].failure;
                auto next = transition(failure, byte);
                while (failure && next == no_id) {
                    charge();
                    failure = machine[failure].failure;
                    next = transition(failure, byte);
                }
                machine[child].failure = next == no_id ? 0 : next;
                const auto target = machine[child].failure;
                machine[child].output = machine[target].terminal == no_id
                    ? machine[target].output : target;
                queue.push_back(child);
            }
        }
        next_word.assign(words.size(), words.size());
        for (const auto& state : machine) {
            charge();
            if (state.terminal != no_id && state.output != no_id)
                next_word[state.terminal] = machine[state.output].terminal;
        }
        byte_rejections = !alphabet.all();
        for (const auto& row : adjacent) byte_rejections |= !row.all();
        node_exclusions = !words.empty();
        for (NodeId id = 0; id != count; ++id)
            if (reachable[id] && (!analysis->length(id).productive ||
                (ast->nodes[id].kind == Kind::CharSet &&
                 !ast->nodes[id].characters.all())))
                node_exclusions = true;
        complete = true;
    }
};


PrivateCuller::PrivateCuller(const Ast& ast, const OccurrenceAnalysis& analysis) {
    try {
        auto compiled = std::make_shared<Compiled>(ast, analysis);
        compiled->compile();
        if (compiled->complete) compiled_ = std::move(compiled);
    } catch (const CompileLimit&) {
        // Optional acceleration declines; authoritative matching is unchanged.
    } catch (const std::bad_alloc&) {
        // Incomplete summaries and their private buffers are released here.
    }
}

bool PrivateCuller::available() const noexcept { return bool(compiled_); }
bool PrivateCuller::can_reject_bytes() const noexcept {
    return compiled_ && compiled_->byte_rejections;
}
bool PrivateCuller::can_exclude_nodes() const noexcept {
    return compiled_ && compiled_->node_exclusions;
}

void PrivateCuller::scan_range(std::string_view subject, std::size_t begin,
                              std::size_t end, const std::atomic_bool& cancel,
                              AtomicExclusions& exclusions, Limits limits,
                              Stats& stats) const {
    stats = {};
    if (!compiled_ || begin > end || end > subject.size() ||
        subject.size() > limits.max_input_bytes) {
        stats.declined = true;
        return;
    }
    const auto& compiled = *compiled_;
    try {
        Work work(cancel, exclusions, limits, stats);
        for (auto position = begin; position != end; ++position) {
            work.step();
            const auto byte = byte_at(subject, position);
            ++stats.bytes_scanned;
            if (!compiled.alphabet[byte]) {
                exclusions.reject_root();
                stats.rejection_reason = RejectionReason::Alphabet;
                stats.completed = true;
                return;
            }
            if (position && !compiled.adjacent[byte_at(subject, position - 1)][byte]) {
                exclusions.reject_root();
                stats.rejection_reason = RejectionReason::Adjacency;
                stats.completed = true;
                return;
            }
        }
        stats.completed = true;
    } catch (const QueryLimit&) {
        stats.declined = true;
    } catch (const Cancelled&) {
        stats.cancelled = true;
    }
}

void PrivateCuller::scan_presence(std::string_view subject,
                                 const std::atomic_bool& cancel,
                                 AtomicExclusions& exclusions, Limits limits,
                                 Stats& stats) const {
    stats = {};
    if (!compiled_ || subject.size() > limits.max_input_bytes ||
        exclusions.node_count() < compiled_->ast->nodes.size()) {
        stats.declined = true;
        return;
    }
    const auto& compiled = *compiled_;
    const auto& ast = *compiled.ast;
    stats.literal_fragments = compiled.words.size();
    try {
        Work work(cancel, exclusions, limits, stats);
        std::bitset<256> seen;
        std::vector<unsigned char> found(compiled.words.size(), false);
        // Disjoint-set successor links retire output words after their first
        // occurrence. Dense a/aa/aaa patterns do not replay old hit lists.
        const auto sentinel = compiled.words.size();
        std::vector<std::size_t> successor(sentinel + 1);
        for (std::size_t word = 0; word <= sentinel; ++word) {
            work.step();
            successor[word] = word;
        }
        const auto unseen = [&](std::size_t word) {
            auto root = word;
            while (successor[root] != root) {
                work.step();
                root = successor[root];
            }
            while (successor[word] != word) {
                work.step();
                const auto next = successor[word];
                successor[word] = root;
                word = next;
            }
            return root;
        };
        std::size_t state = 0;
        for (std::size_t position = 0; position != subject.size(); ++position) {
            work.step();
            const auto byte = byte_at(subject, position);
            ++stats.bytes_scanned;
            if (!compiled.alphabet[byte]) {
                exclusions.reject_root();
                stats.rejection_reason = RejectionReason::Alphabet;
                stats.completed = true;
                return;
            }
            if (position && !compiled.adjacent[byte_at(subject, position - 1)][byte]) {
                exclusions.reject_root();
                stats.rejection_reason = RejectionReason::Adjacency;
                stats.completed = true;
                return;
            }
            seen[byte] = true;
            // Once all words are present, only alphabet/pair checks remain.
            if (stats.literals_found == sentinel) continue;
            auto next = compiled.transition(state, byte);
            while (state && next == no_id) {
                work.step();
                state = compiled.machine[state].failure;
                next = compiled.transition(state, byte);
            }
            state = next == no_id ? 0 : next;
            const auto output = compiled.machine[state].terminal == no_id
                ? compiled.machine[state].output : state;
            auto word = unseen(output == no_id ? sentinel : compiled.machine[output].terminal);
            while (word != sentinel) {
                work.step();
                found[word] = true;
                ++stats.literals_found;
                successor[word] = unseen(compiled.next_word[word]);
                word = successor[word];
            }
        }

        // A private least fixed point; false means only unresolved until every
        // positive consequence has propagated. Literal absence is now final.
        work.poll();
        const auto count = ast.nodes.size();
        std::vector<unsigned char> live(count, false);
        std::vector<std::size_t> remaining(count, 0);
        std::deque<NodeId> queue;
        const auto activate = [&](NodeId id) {
            if (!live[id]) { live[id] = true; queue.push_back(id); }
        };
        for (NodeId id = 0; id != count; ++id) {
            work.step();
            if (!compiled.reachable[id]) continue;
            const auto& node = ast.nodes[id];
            if (compiled.literal[id] != no_id) {
                if (found[compiled.literal[id]]) activate(id);
                continue;
            }
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: activate(id); break;
            case Kind::CharSet:
                if ((node.characters & seen).any()) activate(id);
                break;
            case Kind::Concat:
                remaining[id] = node.children.size();
                if (node.children.empty()) activate(id);
                break;
            case Kind::Repeat:
                if (node.lower == 0) activate(id);
                break;
            default: break;
            }
        }
        while (!queue.empty()) {
            work.step();
            const auto child = queue.front(); queue.pop_front();
            for (const auto parent : compiled.parents[child]) {
                work.step();
                if (live[parent] || compiled.literal[parent] != no_id) continue;
                const auto& node = ast.nodes[parent];
                if (node.kind == Kind::Concat) {
                    if (remaining[parent] && --remaining[parent] == 0) activate(parent);
                } else if (node.kind == Kind::Alt || node.kind == Kind::Capture ||
                           node.kind == Kind::Call || node.kind == Kind::Repeat) activate(parent);
            }
        }

        // Only monotone certificates cross the ownership boundary. A stop or
        // exhausted budget during publication leaves a sound partial subset.
        work.poll();
        if (!live[ast.root]) {
            exclusions.reject_root();
            stats.rejection_reason = RejectionReason::Productivity;
            stats.completed = true;
            return;
        }
        for (NodeId base = 0; base < count; base += 64) {
            std::uint64_t mask = 0;
            const auto end = std::min(count, base + 64);
            for (NodeId id = base; id != end; ++id) {
                work.step();
                if (compiled.reachable[id] && !live[id])
                    mask |= std::uint64_t{1} << (id - base);
            }
            if (mask)
                stats.inactive_nodes += std::popcount(exclusions.exclude_word(base / 64, mask));
        }
        stats.completed = true;
    } catch (const QueryLimit&) {
        stats.declined = true;
    } catch (const Cancelled&) {
        stats.cancelled = true;
    } catch (const std::bad_alloc&) {
        stats.declined = true;
    }
}

} // namespace crank::detail
