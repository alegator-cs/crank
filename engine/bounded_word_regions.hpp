#pragma once
#include "occurrence_analysis.hpp"
#include "exact_program_statistics.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstdint>
#include <functional>
#include <stdexcept>

namespace crank::detail {



// Source-derived LL(2) certificate for closed Boolean root membership. The
// ordinary one-byte certificate remains the first tier. Only conflicting
// FIRST1 tokens acquire a second-byte row; EOF is token 256, never a byte.
class BoundedWordRegion {
    friend class LoweredBoundedWordProgram;
    using Bits = std::bitset<257>;
    using Action = std::uint16_t;
    static constexpr Action none = 65535, conflict = 65534, row_base = 512;
    struct Row {
        bool single = false; // Complete one-token word, not a truncated prefix.
        Bits second;
        bool operator==(const Row&) const = default;
    };
    struct Language {
        bool productive = false, nullable = false;
        Bits first;
        Row row;
    };
    struct Choice { std::array<Action, 257> token; Choice() { token.fill(none); } };
    struct Limited {};
    std::vector<NodeId> targets_;
    std::vector<Action> indices_;
    std::vector<Choice> choices_, rows_;
    static Bits bytes(const std::bitset<256>& source) {
        Bits result;
        for (unsigned byte = 0; byte < 256; ++byte) if (source[byte]) result.set(byte);
        return result;
    }
    void charge(std::size_t count = 1) {
        if (count > 131072 - analysis_steps) throw Limited{};
        analysis_steps += count;
    }
    Language product(const Language& left, const Language& right) {
        charge(); Language result;
        if (!left.productive || !right.productive) return result;
        result.productive = true; result.nullable = left.nullable && right.nullable;
        result.first = left.first;
        if (left.nullable) result.first |= right.first;
        result.row.second = left.row.second;
        if (left.row.single) result.row.second |= right.first;
        if (left.nullable) result.row.second |= right.row.second;
        result.row.single = (left.row.single && right.nullable) || (left.nullable && right.row.single);
        return result;
    }
    bool add(Row& to, const Row& from) {
        charge(); const auto before = to;
        to.single |= from.single; to.second |= from.second;
        return to != before;
    }
    Language unite(Language left, const Language& right) {
        charge(); left.productive |= right.productive; left.nullable |= right.nullable;
        left.first |= right.first; left.row.single |= right.row.single; left.row.second |= right.row.second;
        return left;
    }
    Language powers(const Language& body, std::size_t lower, std::optional<std::size_t> upper) {
        // At width two powers stabilize after two factors, also for a nullable
        // body (epsilon pads every longer power). Complete one-token words
        // remain distinguishable from two-token prefixes throughout.
        Language power; power.productive = power.nullable = true;
        Language result;
        const auto lo = std::min<std::size_t>(lower, 2);
        const auto hi = upper ? std::min<std::size_t>(*upper, 2) : 2;
        for (std::size_t count = 0; count <= hi; ++count) {
            if (count >= lo) result = unite(result, power);
            if (count < hi) power = product(power, body);
        }
        return result;
    }
    template<class Analysis>
    void build(const Ast& ast, const Analysis& analysis, bool allow_acyclic) {
        const auto n = ast.nodes.size();
        if (n > row_base) throw Limited{};
        if (ast.root >= n) return;
        bool calls = false;
        std::size_t source_edges = 0;
        // Defend the isolated constructor too; production passes checked_ast.
        for (const auto& node : ast.nodes) {
            charge();
            // Bound even unreachable/public AST edge vectors before walking
            // them: graph reachability limits alone cannot cap validation.
            if (node.children.size() > 4096 - source_edges) throw Limited{};
            source_edges += node.children.size();
            charge(node.children.size());
            switch (node.kind) {
            case Kind::Empty: case Kind::CharSet: case Kind::Concat: case Kind::Alt:
            case Kind::Repeat: case Kind::Capture: case Kind::Call: case Kind::Define: break;
            default: return;
            }
            if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id) return;
            if ((node.kind == Kind::Capture || node.kind == Kind::Repeat) && node.children.size() != 1) return;
            if (node.kind == Kind::Repeat && node.upper && *node.upper < node.lower) return;
            if (node.kind == Kind::Call) { calls = true; if (node.call_target >= n) return; }
            for (const auto child : node.children) if (child >= n) return;
        }
        if (!calls && !allow_acyclic) return;
        std::vector<NodeId> reachable, pending{ast.root};
        std::vector<unsigned char> seen(n);
        std::vector<std::vector<NodeId>> graph(n), zero(n);
        std::size_t edge_count = 0;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (seen[id]) continue;
            seen[id] = 1; reachable.push_back(id); charge();
            const auto& node = ast.nodes[id];
            if (node.kind == Kind::Call) graph[id] = zero[id] = {node.call_target};
            else if (node.kind == Kind::Alt || node.kind == Kind::Capture) graph[id] = zero[id] = node.children;
            else if (node.kind == Kind::Concat) {
                graph[id] = node.children;
                for (const auto child : node.children) {
                    zero[id].push_back(child);
                    if (!analysis.boundary(child).nullable) break;
                }
            } else if (node.kind == Kind::Repeat && (!node.upper || *node.upper)) graph[id] = zero[id] = node.children;
            edge_count += graph[id].size(); if (edge_count > 4096) throw Limited{};
            for (const auto child : graph[id]) pending.push_back(child);
        }
        const auto cyclic = [&](const auto& links) {
            std::vector<unsigned char> color(n);
            std::function<bool(NodeId)> visit = [&](NodeId id) {
                charge(); if (color[id]) return color[id] == 1;
                color[id] = 1;
                for (const auto child : links[id]) if (visit(child)) return true;
                color[id] = 2; return false;
            };
            for (const auto id : reachable) if (visit(id)) return true;
            return false;
        };
        if ((!allow_acyclic && !cyclic(graph)) || cyclic(zero)) return;
        std::vector<Language> base(n);
        for (const auto id : reachable) {
            base[id].productive = analysis.length(id).productive;
            base[id].nullable = analysis.boundary(id).nullable;
            base[id].first = bytes(analysis.boundary(id).first);
        }
        std::vector<Bits> follow(n); follow[ast.root].set(256);
        bool changed = true;
        while (changed) {
            changed = false; ++follow1_rounds;
            const auto add_first = [&](NodeId id, const Bits& source) {
                charge(); const auto before = follow[id]; follow[id] |= source;
                changed |= before != follow[id];
            };
            for (const auto id : reachable) {
                charge(); const auto& node = ast.nodes[id];
                if (node.kind == Kind::Concat) {
                    auto suffix = follow[id];
                    for (auto i = node.children.size(); i-- > 0;) {
                        const auto child = node.children[i]; add_first(child, suffix);
                        suffix = base[child].nullable ? suffix | base[child].first : base[child].first;
                    }
                } else if (node.kind == Kind::Repeat && (!node.upper || *node.upper)) {
                    auto suffix = follow[id]; const auto child = node.children.front();
                    if (!node.upper || *node.upper > 1) suffix |= base[child].first;
                    add_first(child, suffix);
                } else for (const auto child : graph[id]) add_first(child, follow[id]);
            }
        }
        indices_.assign(n, none);
        Bits conflicting_tokens;
        for (const auto id : reachable) {
            const auto& node = ast.nodes[id];
            const bool repeat_choice = node.kind == Kind::Repeat && (!node.upper || *node.upper) &&
                (!node.upper || *node.upper != node.lower);
            if (node.kind == Kind::Repeat && (!node.upper || *node.upper) && base[node.children.front()].nullable) return;
            if (node.kind != Kind::Alt && !repeat_choice) continue;
            if (choices_.size() == 128) throw Limited{};
            indices_[id] = static_cast<Action>(choices_.size()); choices_.emplace_back();
            auto& choice = choices_.back();
            const auto insert = [&](const Bits& keys, Action target) {
                for (unsigned a = 0; a <= 256; ++a) if (keys[a]) {
                    auto& value = choice.token[a];
                    if (value == none) value = target;
                    else if (value != target) { value = conflict; conflicting_tokens.set(a); }
                }
            };
            if (node.kind == Kind::Alt) {
                for (const auto child : node.children) {
                    auto tokens = base[child].first;
                    if (base[child].nullable) tokens |= follow[id];
                    insert(tokens, static_cast<Action>(child));
                }
            } else { insert(base[node.children.front()].first, 1); insert(follow[id], 0); }
        }
        first1_steps = analysis_steps;
        if (conflicting_tokens[256]) return; // Two epsilon choices cannot be separated at EOF.
        if (conflicting_tokens.count() > 8) throw Limited{};
        for (unsigned a = 0; a < 256; ++a) if (conflicting_tokens[a]) {
            ++refined_bytes;
            const auto first_start = analysis_steps;
            auto prefixes = base;
            bool progress = true;
            while (progress) {
                progress = false; ++first2_rounds;
                for (const auto id : reachable) {
                    if (!base[id].first[a]) continue; // FIRST1-directed source slice.
                    charge(); const auto& node = ast.nodes[id]; Language result;
                    switch (node.kind) {
                    case Kind::CharSet: result.row.single = node.characters[a]; break;
                    case Kind::Call: result.row = prefixes[node.call_target].row; break;
                    case Kind::Capture: result.row = prefixes[node.children.front()].row; break;
                    case Kind::Alt:
                        for (const auto child : node.children) add(result.row, prefixes[child].row);
                        break;
                    case Kind::Concat:
                        result.productive = result.nullable = true;
                        for (const auto child : node.children) result = product(result, prefixes[child]);
                        break;
                    case Kind::Repeat: result = powers(prefixes[node.children.front()], node.lower, node.upper); break;
                    default: break;
                    }
                    progress |= add(prefixes[id].row, result.row);
                }
            }
            for (const auto id : reachable) if (base[id].first[a]) ++first_rows;
            const auto first_done = analysis_steps;
            first2_steps += first_done - first_start;
            std::vector<Language> contexts(n);
            for (const auto id : reachable) { contexts[id].first = follow[id]; contexts[id].productive = follow[id].any(); }
            progress = true;
            while (progress) {
                progress = false; ++follow2_rounds;
                for (const auto id : reachable) {
                    charge(); const auto& node = ast.nodes[id];
                    const auto send = [&](NodeId child, const Language& suffix) {
                        progress |= add(contexts[child].row, suffix.row);
                    };
                    if (node.kind == Kind::Concat) {
                        auto suffix = contexts[id];
                        for (auto i = node.children.size(); i-- > 0;) {
                            const auto child = node.children[i]; send(child, suffix);
                            suffix = product(prefixes[child], suffix);
                        }
                    } else if (node.kind == Kind::Repeat && (!node.upper || *node.upper)) {
                        const auto child = node.children.front();
                        const auto count = node.upper ? std::optional<std::size_t>(*node.upper - 1) : std::nullopt;
                        send(child, product(powers(prefixes[child], 0, count), contexts[id]));
                    } else for (const auto child : graph[id]) send(child, contexts[id]);
                }
            }
            follow2_steps += analysis_steps - first_done;
            for (const auto id : reachable) if (follow[id][a]) ++follow_rows;
            for (const auto id : reachable) {
                if (indices_[id] == none || choices_[indices_[id]].token[a] != conflict) continue;
                if (rows_.size() == 128) throw Limited{};
                Choice row; const auto& node = ast.nodes[id];
                const auto insert = [&](const Bits& keys, Action target) {
                    for (unsigned b = 0; b <= 256; ++b) if (keys[b]) {
                        auto& value = row.token[b];
                        if (value != none && value != target) return false;
                        value = target;
                    }
                    return true;
                };
                if (node.kind == Kind::Alt) {
                    for (const auto child : node.children)
                        if (!insert(product(prefixes[child], contexts[id]).row.second, static_cast<Action>(child))) return;
                } else {
                    const auto more = powers(prefixes[node.children.front()], 1, node.upper);
                    if (!insert(product(more, contexts[id]).row.second, 1) || !insert(contexts[id].row.second, 0)) return;
                }
                choices_[indices_[id]].token[a] = static_cast<Action>(row_base + rows_.size());
                rows_.push_back(std::move(row));
            }
        }
        targets_.resize(n);
        for (const auto id : reachable) {
            auto target = id;
            for (std::size_t count = 0; count < n; ++count) {
                charge(); const auto& node = ast.nodes[target];
                if (node.kind == Kind::Call) target = node.call_target;
                else if (node.kind == Kind::Capture || ((node.kind == Kind::Concat || node.kind == Kind::Alt) && node.children.size() == 1)) target = node.children.front();
                else break;
            }
            targets_[id] = target;
        }
        decision_rows = rows_.size();
        certified = true;
    }
    Action choose(NodeId node, std::string_view input, std::size_t position, std::size_t end) const {
        const auto a = position < end ? static_cast<unsigned char>(input[position]) : 256U;
        auto value = choices_[indices_[node]].token[a];
        if (value >= row_base && value != none) {
            const auto b = position < end && end - position > 1 ? static_cast<unsigned char>(input[position + 1]) : 256U;
            value = rows_[value - row_base].token[b];
        }
        return value;
    }
public:
    bool certified = false, limit_reached = false;
    std::size_t analysis_steps = 0, first1_steps = 0, first2_steps = 0, follow2_steps = 0;
    std::size_t follow1_rounds = 0, first2_rounds = 0, follow2_rounds = 0;
    std::size_t refined_bytes = 0, first_rows = 0, follow_rows = 0, decision_rows = 0;
    struct Runtime { std::size_t steps = 0, frames = 0, maximum_frames = 0; };
    template<class Analysis>
    BoundedWordRegion(const Ast& ast, const Analysis& analysis,
                      bool prior_certificate = false, bool allow_acyclic = false) {
        if (prior_certificate) return;
        try { build(ast, analysis, allow_acyclic); }
        catch (const Limited&) { limit_reached = true; }
        catch (const std::bad_alloc&) { limit_reached = true; }
        if (!certified) {
            std::vector<NodeId>().swap(targets_); std::vector<Action>().swap(indices_);
            std::vector<Choice>().swap(choices_); std::vector<Choice>().swap(rows_);
        }
    }
    [[nodiscard]] std::size_t retained_bytes() const {
        return sizeof(*this) + targets_.capacity() * sizeof(NodeId) + indices_.capacity() * sizeof(Action) +
            (choices_.capacity() + rows_.capacity()) * sizeof(Choice);
    }
    // tick() charges/polls the caller's step budget. frame() charges one new
    // continuation state. Either callback may throw the caller's resource or
    // cancellation exception; exhaustion is never converted into rejection.
    template<class Tick, class FrameCharge>
    [[nodiscard]] std::optional<std::size_t> execute(const Ast& ast, std::string_view input,
            std::size_t begin, std::size_t end, Tick&& tick, FrameCharge&& frame_charge,
            Runtime* statistics = nullptr) const {
        if (!certified) throw std::logic_error("uncertified bounded-word region");
        if (begin > end || end > input.size()) throw std::out_of_range("bounded-word demand");
        Runtime local; auto& stats = statistics ? *statistics : local; stats = {};
        struct Frame { NodeId node; std::size_t next; };
        std::vector<Frame> stack;
        const auto step = [&] { tick(); ++stats.steps; };
        const auto push = [&](NodeId node, std::size_t next) {
            frame_charge(); stack.push_back({node, next}); ++stats.frames;
            stats.maximum_frames = std::max(stats.maximum_frames, stack.size());
        };
        auto position = begin; NodeId current = ast.root;
        const auto again = [&](NodeId id, std::size_t count) {
            const auto& node = ast.nodes[id];
            if (node.upper && count == *node.upper) return false;
            if (count < node.lower) return true;
            return choose(id, input, position, end) == 1;
        };
        for (;;) {
            step(); current = targets_[current]; const auto& node = ast.nodes[current];
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: break;
            case Kind::CharSet:
                if (position == end || !node.characters[static_cast<unsigned char>(input[position])]) return {};
                ++position; break;
            case Kind::Alt: {
                const auto target = choose(current, input, position, end);
                if (target == none) return {};
                current = target; continue;
            }
            case Kind::Concat:
                if (node.children.empty()) break;
                if (node.children.size() > 1) push(current, 1);
                current = node.children.front(); continue;
            case Kind::Repeat:
                if (!again(current, 0)) break;
                push(current, 1); current = node.children.front(); continue;
            case Kind::Call: current = node.call_target; continue;
            case Kind::Capture: current = node.children.front(); continue;
            case Kind::Reference: throw std::logic_error("stateful bounded-word region");
            }
            bool resumed = false;
            while (!stack.empty()) {
                step(); auto& frame = stack.back(); const auto& parent = ast.nodes[frame.node];
                if (parent.kind == Kind::Concat) {
                    current = parent.children[frame.next++];
                    if (frame.next == parent.children.size()) stack.pop_back();
                    resumed = true; break;
                }
                if (again(frame.node, frame.next)) {
                    ++frame.next; current = parent.children.front(); resumed = true; break;
                }
                if (frame.next < parent.lower) return {};
                stack.pop_back();
            }
            if (!resumed) return position == end ? std::optional<std::size_t>{position} : std::nullopt;
        }
    }
};
// One reusable positive or negative source result. Merely reading statistics
// never constructs a certificate; publication uses release/acquire ordering.
class BoundedWordRegionCache {
    const Ast& ast_;
    const OccurrenceAnalysis& analysis_;
    const bool prior_certificate_;
    mutable std::once_flag once_;
    mutable std::atomic_bool ready_{false};
    mutable std::unique_ptr<BoundedWordRegion> program_;
public:
    BoundedWordRegionCache(const Ast& ast, const OccurrenceAnalysis& analysis, bool prior_certificate = false)
        : ast_(ast), analysis_(analysis), prior_certificate_(prior_certificate) {}
    const BoundedWordRegion* certificate() const {
        if (prior_certificate_) return nullptr;
        if (!ready_.load(std::memory_order_acquire)) {
            std::call_once(once_, [&] {
                try { program_ = std::make_unique<BoundedWordRegion>(ast_, analysis_); }
                catch (const std::bad_alloc&) {}
                ready_.store(true, std::memory_order_release);
            });
        }
        return program_ && program_->certified ? program_.get() : nullptr;
    }
    BoundedWordPreparationStatistics statistics() const {
        if (!ready_.load(std::memory_order_acquire)) return {};
        BoundedWordPreparationStatistics result; result.attempted = true;
        if (!program_) { result.limited = true; return result; }
        result.certified = program_->certified; result.limited = program_->limit_reached;
        result.analysis_steps = program_->analysis_steps; result.refined_bytes = program_->refined_bytes;
        result.decision_rows = program_->decision_rows; result.retained_bytes = program_->retained_bytes();
        return result;
    }
};
} // namespace crank::detail
