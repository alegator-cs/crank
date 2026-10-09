#include "input_constraints.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <unordered_map>
#include <utility>

namespace crank::detail {
namespace {

// Compilation is optional and bounded independently of the recognizer. Sparse
// automaton edges avoid a 256-entry transition table for each literal byte.
constexpr std::size_t max_nodes = 65'536;
constexpr std::size_t max_edges = 262'144;
constexpr std::size_t max_compile_steps = 16'000'000;
constexpr std::size_t max_literal_bytes = 262'144;
constexpr std::size_t max_literal_size = 65'536;
constexpr std::size_t max_literals = 4'096;

struct CompileLimit {};
struct QueryLimit {};

bool plain(const Ast& ast) {
    return !ast.has_references && !ast.has_counts && !ast.has_extended &&
           !ast.has_occurrence_paths;
}

unsigned char byte_at(std::string_view text, std::size_t position) {
    return static_cast<unsigned char>(text[position]);
}

} // namespace

struct InputConstraints::Compiled {
    const Ast* ast;
    const OccurrenceAnalysis* analysis;
    bool complete = false;
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

    Compiled(const Ast& source, const OccurrenceAnalysis& summaries)
        : ast(&source), analysis(&summaries) {}

    std::size_t transition(std::size_t state, unsigned char byte) const {
        const auto& edges = machine[state].edges;
        const auto found = std::lower_bound(edges.begin(), edges.end(), byte,
            [](const auto& edge, unsigned char key) { return edge.first < key; });
        return found != edges.end() && found->first == byte ? found->second : no_id;
    }

    void compile() {
        if (!plain(*ast) || ast->nodes.size() > max_nodes || ast->root >= ast->nodes.size())
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
        complete = true;
    }
};

InputConstraints::InputConstraints(const Ast& ast, const OccurrenceAnalysis& analysis) {
    auto compiled = std::make_shared<Compiled>(ast, analysis);
    try { compiled->compile(); }
    catch (const CompileLimit&) {
        // Do not retain a large unfinished automaton beside the fallback engine.
        compiled = std::make_shared<Compiled>(ast, analysis);
    }
    // Allocation failures remain ordinary allocation failures, rather than
    // silently exhausting the recognizer's separately reported resource budget.
    compiled_ = std::move(compiled);
}

InputConstraints::Index InputConstraints::build(std::string_view subject, Limits limits) const {
    Index result;
    result.compiled_ = compiled_;
    result.subject_ = subject;
    const auto& compiled = *compiled_;
    const auto& ast = *compiled.ast;
    if (compiled.complete) result.stats_.literal_fragments = compiled.words.size();
    if (!compiled.complete || subject.size() > limits.max_input_bytes) {
        result.stats_.declined = true;
        return result;
    }
    const auto charge = [&](std::size_t n = 1) {
        if (n > limits.max_steps - result.stats_.steps) throw QueryLimit{};
        result.stats_.steps += n;
    };
    std::bitset<256> seen;
    result.starts_.resize(compiled.words.size());
    try {
        std::size_t state = 0;
        for (std::size_t position = 0; position != subject.size(); ++position) {
            charge();
            const auto byte = byte_at(subject, position);
            ++result.stats_.bytes_scanned;
            if (!compiled.alphabet[byte]) {
                result.rejected_ = true;
                result.stats_.rejection_reason = "alphabet";
                return result;
            }
            if (position && !compiled.adjacent[byte_at(subject, position - 1)][byte]) {
                result.rejected_ = true;
                result.stats_.rejection_reason = "adjacency";
                return result;
            }
            seen[byte] = true;
            auto next = compiled.transition(state, byte);
            while (state && next == no_id) {
                charge();
                state = compiled.machine[state].failure;
                next = compiled.transition(state, byte);
            }
            state = next == no_id ? 0 : next;
            auto output = compiled.machine[state].terminal == no_id
                ? compiled.machine[state].output : state;
            while (output != no_id) {
                charge();
                if (result.stats_.literal_hits == limits.max_occurrences) throw QueryLimit{};
                const auto word = compiled.machine[output].terminal;
                result.starts_[word].push_back(position + 1 - compiled.words[word].size());
                ++result.stats_.literal_hits;
                output = compiled.machine[output].output;
            }
        }
        result.scanned_ = true;

        // Least fixed point of input-relative productivity. Each false->true
        // transition is propagated once. In particular, no unfinished recursive
        // cell is ever interpreted as a proved negative.
        const auto count = ast.nodes.size();
        result.live_.assign(count, false);
        std::vector<std::size_t> remaining(count, 0);
        std::deque<NodeId> queue;
        const auto activate = [&](NodeId id) {
            if (!result.live_[id]) { result.live_[id] = true; queue.push_back(id); }
        };
        for (NodeId id = 0; id != count; ++id) {
            charge();
            if (!compiled.reachable[id]) continue;
            const auto& node = ast.nodes[id];
            if (compiled.literal[id] != no_id) {
                if (!result.starts_[compiled.literal[id]].empty()) activate(id);
                continue;
            }
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: case Kind::Reference: activate(id); break;
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
            const auto child = queue.front(); queue.pop_front();
            for (const auto parent : compiled.parents[child]) {
                charge();
                if (result.live_[parent] || compiled.literal[parent] != no_id) continue;
                const auto& node = ast.nodes[parent];
                if (node.kind == Kind::Concat) {
                    if (remaining[parent] && --remaining[parent] == 0) activate(parent);
                } else if (node.kind == Kind::Alt || node.kind == Kind::Capture ||
                           node.kind == Kind::Call || node.kind == Kind::Repeat) activate(parent);
            }
        }
        result.productivity_complete_ = true;
        if (!result.live_[ast.root]) {
            result.rejected_ = true;
            result.stats_.rejection_reason = "input-productivity";
        }
        for (NodeId id = 0; id != count; ++id) {
            charge();
            if (compiled.reachable[id] && !result.live_[id]) ++result.stats_.inactive_nodes;
        }
        if (result.rejected_) return result;

        // Linear-time consecutive-run summaries. First find each occurrence's
        // successor using a monotone second cursor, then replace links from
        // right to left with the farthest reachable byte endpoint.
        result.run_ends_.resize(compiled.words.size());
        for (std::size_t word = 0; word != compiled.words.size(); ++word) {
            const auto& starts = result.starts_[word];
            auto& ends = result.run_ends_[word];
            ends.resize(starts.size(), no_id);
            std::size_t next = 0;
            const auto width = compiled.words[word].size();
            for (std::size_t i = 0; i != starts.size(); ++i) {
                charge();
                if (next <= i) next = i + 1;
                while (next < starts.size() && starts[next] < starts[i] + width) {
                    charge();
                    ++next;
                }
                if (next < starts.size() && starts[next] == starts[i] + width) ends[i] = next;
            }
            for (std::size_t i = starts.size(); i-- != 0;) {
                charge();
                ends[i] = ends[i] == no_id ? starts[i] + width : ends[ends[i]];
            }
        }
        result.runs_complete_ = true;
    } catch (const QueryLimit&) {
        result.stats_.declined = true;
        if (!result.scanned_) result.starts_.clear();
        if (!result.productivity_complete_) result.live_.clear();
        result.run_ends_.clear();
    }
    return result;
}

bool InputConstraints::Index::root_possible() const {
    if (rejected_) return false;
    return !productivity_complete_ || live_[compiled_->ast->root];
}

std::optional<std::size_t> InputConstraints::Index::literal_size(NodeId id) const {
    if (!compiled_->complete || id >= compiled_->fixed_length.size() ||
        compiled_->fixed_length[id] == no_id) return {};
    return compiled_->fixed_length[id];
}

const std::vector<std::size_t>* InputConstraints::Index::starts(NodeId id) const {
    if (!scanned_ || id >= compiled_->literal.size() || compiled_->literal[id] == no_id)
        return nullptr;
    return &starts_[compiled_->literal[id]];
}

std::optional<bool> InputConstraints::Index::exact(NodeId id, std::size_t begin,
                                                  std::size_t end) const {
    if (begin > end || end > subject_.size()) return false;
    const auto& ast = *compiled_->ast;
    if (id >= ast.nodes.size() || !plain(ast)) return {};
    const auto& node = ast.nodes[id];
    if (node.kind == Kind::Empty || node.kind == Kind::Define) return begin == end;
    if (node.kind == Kind::CharSet)
        return end - begin == 1 && node.characters[byte_at(subject_, begin)];
    if (const auto* occurrences = starts(id)) {
        if (end - begin != compiled_->fixed_length[id]) return false;
        return std::binary_search(occurrences->begin(), occurrences->end(), begin);
    }
    if (node.kind != Kind::Repeat || node.children.size() != 1 || !runs_complete_) return {};
    if (node.upper && *node.upper < node.lower) return false;
    const auto child = node.children.front();
    const auto width = literal_size(child);
    if (!width) return {};
    if (*width == 0) return begin == end;
    const auto length = end - begin;
    if (length % *width) return false;
    const auto repeats = length / *width;
    if (repeats < node.lower || (node.upper && repeats > *node.upper)) return false;
    if (!repeats) return true;
    const auto* occurrences = starts(child);
    if (!occurrences) return {};
    const auto at = std::lower_bound(occurrences->begin(), occurrences->end(), begin);
    if (at == occurrences->end() || *at != begin) return false;
    const auto index = static_cast<std::size_t>(at - occurrences->begin());
    return run_ends_[compiled_->literal[child]][index] >= end;
}

bool InputConstraints::Index::possible(NodeId id, std::size_t begin,
                                      std::size_t end) const {
    if (begin > end || end > subject_.size()) return false;
    if (id >= compiled_->ast->nodes.size()) return true;
    if (productivity_complete_ && compiled_->reachable[id] && !live_[id]) return false;
    const auto& analysis = *compiled_->analysis;
    if (!analysis.length(id).accepts(end - begin)) return false;
    const auto& boundary = analysis.boundary(id);
    if (begin == end) return boundary.nullable;
    if (!boundary.first[byte_at(subject_, begin)] || !boundary.last[byte_at(subject_, end - 1)])
        return false;
    const auto answer = exact(id, begin, end);
    return !answer || *answer;
}

} // namespace crank::detail
