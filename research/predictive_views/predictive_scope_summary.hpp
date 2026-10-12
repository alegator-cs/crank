#pragma once
#include "predictive_regions.hpp"
#include <utility>

namespace crank::detail {

// Experimental semantic summary, deliberately absent from the live regional
// cache. The caller owns one validated, immutable pure AST and completed facts.
// A completed successful builder for this SAME AST and fact table supplies the
// seed-independent/base premise. A node key alone cannot verify provenance.
// This summary does not certify resource admission, EOF permission, observer
// identity, or an execution result. Existing constructors remain authoritative.
struct PredictiveScopeSummary {
    NodeId entry = no_id;
    std::size_t root_child_count = no_id;
    bool complete = false;
    std::bitset<256> forbidden;
    // Influence-node visits plus attempted edge transfers only. Purity scans
    // and hazard-set reads are excluded. Never substitute this experimental
    // counter for builder analysis_steps, allocations, or elapsed time.
    std::size_t analysis_steps = 0;

    PredictiveScopeSummary() = default;
    template<class Analysis>
    PredictiveScopeSummary(const Ast& ast, const Analysis& analysis,
                           const PredictiveRegion& successful)
        : entry(successful.entry), root_child_count(successful.root_child_count) {
        constexpr std::size_t max_nodes = 4096, max_edges = 16384;
        if (!successful.certified || successful.limit_reached ||
            ast.nodes.size() > max_nodes || entry >= ast.nodes.size()) return;
        // This facility is narrower than the underlying internal builder.
        // Public summary flags are not purity evidence.
        for (const auto& node : ast.nodes) {
            if (node.kind == Kind::Reference || node.bind_count != no_id ||
                node.use_count != no_id || node.count_path != no_id) return;
        }
        if (root_child_count != no_id && (ast.root >= ast.nodes.size() ||
            ast.nodes[ast.root].kind != Kind::Concat ||
            root_child_count > ast.nodes[ast.root].children.size())) return;
        const auto children = [&](NodeId id) {
            return id == ast.root && root_child_count != no_id
                ? root_child_count : ast.nodes[id].children.size();
        };
        // Each influenced node is enqueued once. No extra graph/payload heap
        // allocation, no ordered FOLLOW sweeps, and no mutation of the builder.
        std::bitset<max_nodes> influenced;
        std::array<NodeId, max_nodes> queue;
        std::size_t head = 0, tail = 0, edges = 0;
        const auto add = [&](NodeId id) {
            ++analysis_steps;
            if (++edges > max_edges || id >= ast.nodes.size()) return false;
            if (!influenced[id]) {
                influenced.set(id); queue[tail++] = id;
            }
            return true;
        };
        influenced.set(entry); queue[tail++] = entry;
        while (head != tail) {
            const auto id = queue[head++];
            ++analysis_steps;
            const auto& node = ast.nodes[id];
            if (node.kind == Kind::Alt) {
                std::bitset<256> first;
                bool nullable = false;
                for (const auto child : node.children) {
                    if (child >= ast.nodes.size()) return;
                    const auto& b = analysis.boundary(child);
                    first |= b.first; nullable |= b.nullable;
                }
                if (nullable) forbidden |= first;
            } else if (node.kind == Kind::Repeat && (!node.upper || *node.upper) &&
                       (!node.upper || *node.upper != node.lower)) {
                if (node.children.size() != 1 || node.children.front() >= ast.nodes.size()) return;
                forbidden |= analysis.boundary(node.children.front()).first;
            }
            if (node.kind == Kind::Call) {
                if (!add(node.call_target)) return;
            } else if (node.kind == Kind::Capture || node.kind == Kind::Alt) {
                for (const auto child : node.children) if (!add(child)) return;
            } else if (node.kind == Kind::Concat) {
                // External FOLLOW crosses a child only through a nullable
                // later suffix. A required suffix shields earlier hazards.
                for (auto i = children(id); i-- > 0;) {
                    const auto child = node.children[i];
                    if (!add(child)) return;
                    if (!analysis.boundary(child).nullable) break;
                }
            } else if (node.kind == Kind::Repeat && (!node.upper || *node.upper)) {
                if (node.children.size() != 1 || !add(node.children.front())) return;
            }
        }
        complete = true;
    }
    [[nodiscard]] std::optional<bool> admission(const std::bitset<256>& follow) const {
        if (!complete) return std::nullopt;
        return (follow & forbidden).none();
    }
};

// Optional shadow owner for one immutable AST/fact table. It caches only a
// semantic mask, never a preparation decline or a shared admission record.
// Call after ordinary preparation and under the owner's synchronization.
// Fixed experimental storage is outside production retained-byte accounting.
template<class Analysis>
class PredictiveScopeSummaryCache {
    const Ast& ast_;
    const Analysis& analysis_;
    std::array<PredictiveScopeSummary, 128> summaries_{};
    std::size_t count_ = 0;
public:
    std::size_t builds = 0, analysis_steps = 0;
    PredictiveScopeSummaryCache(const Ast& ast, const Analysis& analysis)
        : ast_(ast), analysis_(analysis) {}
    [[nodiscard]] const PredictiveScopeSummary* summary(const PredictiveRegion& prepared) {
        if (!prepared.certified || prepared.limit_reached) return nullptr;
        for (std::size_t i = 0; i < count_; ++i) {
            if (summaries_[i].entry == prepared.entry &&
                summaries_[i].root_child_count == prepared.root_child_count)
                return &summaries_[i];
        }
        // Bound unsuccessful summary attempts as well as retained successes.
        // A failed attempt is unavailability, never a semantic failure flag.
        if (count_ == summaries_.size() || builds == summaries_.size()) return nullptr;
        PredictiveScopeSummary candidate(ast_, analysis_, prepared);
        ++builds; analysis_steps += candidate.analysis_steps;
        if (!candidate.complete) return nullptr;
        summaries_[count_] = std::move(candidate);
        return &summaries_[count_++];
    }
};
} // namespace crank::detail
