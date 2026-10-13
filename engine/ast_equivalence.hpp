#pragma once

#include "regex_ast.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace crank::detail {

struct EquivalenceStatistics {
    std::size_t work = 0;
    // Active representatives removed beyond transparent-control collapse.
    std::size_t merged_nodes = 0;
    bool limit_reached = false;
    // True after graph/label validation, even if refinement later hits its cap.
    bool eligible = false;
};

namespace ast_equivalence_detail {

// Optional compilation must not turn a long acyclic graph into quadratic
// preparation. Count examined signature words, graph edges and comparisons;
// discard the entire proposed quotient if this fixed allowance is exhausted.
struct Exhausted {};
struct Budget {
    static constexpr std::size_t maximum = 1'000'000;
    std::size_t remaining = maximum;
    EquivalenceStatistics* statistics;
    explicit Budget(EquivalenceStatistics* value) : statistics(value) {}
    ~Budget() { if (statistics) statistics->work = maximum - remaining; }
    void spend(std::size_t amount = 1) {
        if (amount > remaining) throw Exhausted{};
        remaining -= amount;
    }
};

} // namespace ast_equivalence_detail

// Requires the validated pure AST used by Boolean native execution, and the
// idempotent map from pure_control_targets(). Captures are not observable here.
// A stable partition is a structural bisimulation of the finite recursive
// equations: equal labels and equal ordered child classes give equal finite
// unfoldings, hence equal least-fixed-point recognition languages. This is a
// sufficient syntactic test, not a claim to decide arbitrary CFG equivalence.
//
// No partially refined partition is usable: nodes that presently look equal
// can separate at the next round. On any compilation-work cap, preserve the
// original transparent-control map without publishing a single new alias.
[[nodiscard]] inline std::vector<NodeId> pure_ast_equivalent_targets(
    const Ast& ast, const std::vector<NodeId>& transparent_control,
    EquivalenceStatistics* statistics = nullptr) {
    using namespace ast_equivalence_detail;
    using Word = std::uint64_t;
    struct Label {
        // CharSet is the longest label: kind, four masks, child count.
        // Repeat uses five words, pure alias cycles three, other kinds two.
        std::array<Word, 6> words{};
        std::size_t length = 0;
        void push_back(Word value) { words[length++] = value; }
        [[nodiscard]] std::size_t size() const { return length; }
    };
    static_assert(sizeof(NodeId) <= sizeof(Word));
    if (statistics) *statistics = {};
    const auto count = ast.nodes.size();
    constexpr std::size_t maximum_nodes = 16'384;
    if (count > maximum_nodes) {
        if (statistics) statistics->limit_reached = true;
        return transparent_control;
    }
    if (count == 0 || transparent_control.size() != count)
        return transparent_control;

    try {
        Budget budget(statistics);
        std::vector<NodeId> active;
        active.reserve(count);
        for (NodeId id = 0; id < count; ++id) {
            budget.spend();
            const auto target = transparent_control[id];
            const auto& node = ast.nodes[id];
            if (target >= count || transparent_control[target] != target ||
                node.kind == Kind::Reference || node.bind_count != no_id ||
                node.use_count != no_id || node.count_path != no_id)
                return transparent_control;
            for (const auto child : node.children) {
                budget.spend();
                if (child >= count) return transparent_control;
            }
            if (node.kind == Kind::Call && node.call_target >= count)
                return transparent_control;
            if (id == target) active.push_back(id);
        }

        // Local labels are immutable across refinement. In particular, terminal
        // masks are extracted once, not on every recursive refinement round.
        // Only representatives need labels. The active order remains fixed;
        // refinement sorts its separate order vector, never this positional map.
        std::vector<Label> labels(active.size());
        std::vector<std::vector<NodeId>> children(count);
        for (std::size_t index = 0; index < active.size(); ++index) {
            const auto id = active[index];
            budget.spend();
            const auto& node = ast.nodes[id];
            auto& label = labels[index];
            label.push_back(static_cast<Word>(node.kind));
            switch (node.kind) {
            case Kind::CharSet:
                for (std::size_t word = 0; word < 4; ++word) {
                    Word mask = 0;
                    for (std::size_t bit = 0; bit < 64; ++bit) {
                        budget.spend();
                        if (node.characters[word * 64 + bit]) mask |= Word{1} << bit;
                    }
                    label.push_back(mask);
                }
                break;
            case Kind::Repeat:
                if (node.children.size() != 1 ||
                    (node.upper && *node.upper < node.lower)) return transparent_control;
                label.push_back(node.lower);
                label.push_back(node.upper.has_value());
                label.push_back(node.upper.value_or(0));
                [[fallthrough]];
            case Kind::Concat:
            case Kind::Alt:
                for (const auto child : node.children) {
                    budget.spend();
                    children[id].push_back(transparent_control[child]);
                }
                break;
            case Kind::Capture:
            case Kind::Call:
                // A transparent representative of either kind belongs to a
                // pure alias cycle. Leave these cycles as singletons rather
                // than depending on a separate productivity argument here.
                label.push_back(id);
                break;
            case Kind::Empty:
            case Kind::Define:
                break;
            case Kind::Reference:
                return transparent_control;
            default:
                return transparent_control;
            }
            label.push_back(children[id].size());
        }
        if (statistics) statistics->eligible = true;

        std::vector<NodeId> classes(count, 0), next(count, 0);
        std::vector<std::vector<Word>> signatures(count);
        std::vector<NodeId> order = active;
        for (;;) {
            for (std::size_t index = 0; index < active.size(); ++index) {
                const auto id = active[index];
                const auto& label = labels[index];
                budget.spend(label.size() + children[id].size() + 1);
                auto& signature = signatures[id];
                signature.assign(label.words.begin(), label.words.begin() + label.size());
                for (const auto child : children[id]) signature.push_back(classes[child]);
            }
            const auto compare = [&](NodeId left, NodeId right) {
                budget.spend();
                const auto& a = signatures[left];
                const auto& b = signatures[right];
                const auto common = std::min(a.size(), b.size());
                for (std::size_t index = 0; index < common; ++index) {
                    budget.spend();
                    if (a[index] != b[index]) return a[index] < b[index] ? -1 : 1;
                }
                return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
            };
            std::sort(order.begin(), order.end(), [&](NodeId left, NodeId right) {
                return compare(left, right) < 0;
            });

            // The representative is the smallest original node ID. Stable
            // names make an unchanged class vector an exact fixed-point test.
            for (std::size_t begin = 0; begin < order.size();) {
                auto end = begin + 1;
                auto representative = order[begin];
                while (end < order.size() && compare(order[begin], order[end]) == 0) {
                    representative = std::min(representative, order[end]);
                    ++end;
                }
                for (auto index = begin; index < end; ++index) {
                    budget.spend();
                    next[order[index]] = representative;
                }
                begin = end;
            }
            bool stable = true;
            for (const auto id : active) {
                budget.spend();
                if (classes[id] != next[id]) stable = false;
            }
            classes.swap(next);
            if (!stable) continue;

            auto result = transparent_control;
            std::size_t merged = 0;
            for (NodeId id = 0; id < count; ++id) {
                budget.spend();
                result[id] = classes[transparent_control[id]];
                if (transparent_control[id] == id && result[id] != id) ++merged;
            }
            if (statistics) statistics->merged_nodes = merged;
            return result;
        }
    } catch (const Exhausted&) {
        if (statistics) statistics->limit_reached = true;
        return transparent_control;
    }
}

} // namespace crank::detail
