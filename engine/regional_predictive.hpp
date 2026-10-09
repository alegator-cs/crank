#pragma once
#include "exact_end_spines.hpp"
#include "predictive_regions.hpp"
#include "regional_statistics.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace crank::detail {

// An admitted endpoint either precedes a listed byte or is the subject EOF.
// This is a necessary property of ALL source continuations in one closed,
// pure full-root Boolean query. It is not the unrestricted producer language.
struct RegionalConsumerContract {
    std::bitset<256> bytes;
    bool eof = false;
    bool operator==(const RegionalConsumerContract&) const = default;
    [[nodiscard]] bool accepts(std::string_view subject, std::size_t end) const {
        return end < subject.size() ? bytes[static_cast<unsigned char>(subject[end])]
             : end == subject.size() && eof;
    }
};

// AstMachineProgram owns the validated immutable Ast, analysis, and control
// arrays for longer than this cache. Preparation is lazy and synchronized;
// readers retain immutable shared certificates after the mutex is released.
// No occurrence positions or live native cells enter the preparation cache.
class RegionalPredictiveCache {
    const Ast& ast_;
    const OccurrenceAnalysis& analysis_;
    const std::vector<NodeId>& control_;
    const std::vector<NodeId>& equivalent_control_;
    mutable std::once_flag initialized_;
    mutable std::atomic_bool initialization_complete_{false};
    mutable std::mutex mutex_;
    mutable bool valid_ = false, limited_ = false;
    mutable std::size_t work_ = 0;
    mutable std::vector<unsigned char> reachable_;
    mutable std::vector<RegionalConsumerContract> contracts_;
    mutable std::vector<std::size_t> contract_ids_;
    mutable std::vector<std::shared_ptr<const PredictiveRegion>> predictive_, exact_;
    mutable std::vector<std::shared_ptr<const ExactEndSpines>> spines_;
    mutable std::vector<unsigned char> tried_predictive_, tried_exact_, tried_spines_;
    // Only permanently declined entries bypass the mutex. Positive shared_ptr
    // slots retain their existing synchronized access; no query owns a copy
    // of these source-bounded admission tables.
    mutable std::unique_ptr<std::atomic_bool[]> declined_predictive_, declined_exact_, declined_spines_;
    mutable RegionalPreparationStatistics statistics_;
    static constexpr std::size_t max_preparations = 128;
    static constexpr std::size_t max_retained_bytes = 8 * 1024 * 1024;
    static constexpr std::size_t max_preparation_work = 4'194'304;

    bool admit_preparation() const {
        // One admitted constructor has its own hard work cap (at most 1 Mi
        // steps), so total cache analysis is bounded by this threshold plus
        // one constructor. Declined nodes are remembered just like successes.
        if (statistics_.predictive_builds + statistics_.spine_builds >= max_preparations ||
            statistics_.analysis_steps >= max_preparation_work ||
            statistics_.retained_bytes >= max_retained_bytes) {
            ++statistics_.limit_declines; ++statistics_.declined; return false;
        }
        return true;
    }
    bool retain(std::size_t bytes) const {
        if (bytes > max_retained_bytes - statistics_.retained_bytes) {
            ++statistics_.limit_declines; ++statistics_.declined; return false;
        }
        statistics_.retained_bytes += bytes; ++statistics_.retained_certificates;
        return true;
    }

    struct ContractHash {
        std::size_t operator()(const RegionalConsumerContract& c) const {
            return std::hash<std::bitset<256>>{}(c.bytes) ^ (c.eof ? 0x9e3779b9U : 0);
        }
    };
    void initialize() const {
        constexpr std::size_t max_nodes = 4096, max_edges = 16384, max_work = 1'048'576;
        const auto n = ast_.nodes.size();
        if (n > max_nodes) { limited_ = true; return; }
        if (ast_.root >= n || control_.size() != n || equivalent_control_.size() != n) return;
        reachable_.assign(n, 0);
        std::vector<NodeId> reachable, pending{ast_.root};
        std::vector<std::vector<NodeId>> graph(n);
        std::size_t edges = 0;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (id >= n) return;
            if (reachable_[id]) continue;
            reachable_[id] = 1; reachable.push_back(id);
            const auto& node = ast_.nodes[id];
            if (node.kind == Kind::Reference || node.bind_count != no_id ||
                node.use_count != no_id || node.count_path != no_id) return;
            if (node.kind == Kind::Call) graph[id].push_back(node.call_target);
            else if (node.kind == Kind::Concat || node.kind == Kind::Alt || node.kind == Kind::Capture ||
                     (node.kind == Kind::Repeat && (!node.upper || *node.upper))) graph[id] = node.children;
            if (graph[id].size() > max_edges - edges) { limited_ = true; return; }
            edges += graph[id].size();
            for (const auto child : graph[id]) pending.push_back(child);
        }
        std::vector<RegionalConsumerContract> follow(n);
        follow[ast_.root].eof = true;
        bool changed = true;
        while (changed) {
            changed = false;
            const auto add = [&](NodeId id, const RegionalConsumerContract& incoming) {
                if (++work_ > max_work) return false;
                auto& value = follow[id];
                const auto bytes = value.bytes | incoming.bytes;
                const bool eof = value.eof || incoming.eof;
                if (value.bytes != bytes || value.eof != eof) {
                    value.bytes = bytes; value.eof = eof; changed = true;
                }
                return true;
            };
            for (const auto id : reachable) {
                const auto& node = ast_.nodes[id];
                if (node.kind == Kind::Concat) {
                    auto suffix = follow[id];
                    for (auto i = node.children.size(); i-- > 0;) {
                        const auto child = node.children[i];
                        if (!add(child, suffix)) { limited_ = true; return; }
                        const auto& boundary = analysis_.boundary(child);
                        suffix.bytes = boundary.nullable ? suffix.bytes | boundary.first : boundary.first;
                        suffix.eof = suffix.eof && boundary.nullable;
                    }
                } else if (node.kind == Kind::Repeat && (!node.upper || *node.upper)) {
                    auto suffix = follow[id];
                    if (!node.upper || *node.upper > 1) suffix.bytes |= analysis_.boundary(node.children.front()).first;
                    if (!add(node.children.front(), suffix)) { limited_ = true; return; }
                } else {
                    for (const auto child : graph[id])
                        if (!add(child, follow[id])) { limited_ = true; return; }
                }
            }
        }
        // Runtime control collapse can share two source entries. Union every
        // original closed contract at both representatives before certifying.
        // This does not require re-closing source FOLLOW: regional compilation
        // independently closes the whole called subgraph under the union seed.
        contracts_ = follow;
        for (const auto id : reachable) {
            for (const auto representative : {control_[id], equivalent_control_[id]}) {
                if (representative >= n) return;
                contracts_[representative].bytes |= follow[id].bytes;
                contracts_[representative].eof = contracts_[representative].eof || follow[id].eof;
                reachable_[representative] = 1;
            }
        }
        contract_ids_.assign(n, no_id);
        std::unordered_map<RegionalConsumerContract, std::size_t, ContractHash> interned;
        for (NodeId id = 0; id < n; ++id) if (reachable_[id]) {
            auto [entry, inserted] = interned.emplace(contracts_[id], interned.size());
            contract_ids_[id] = entry->second;
        }
        predictive_.resize(n); exact_.resize(n); spines_.resize(n);
        tried_predictive_.assign(n, 0); tried_exact_.assign(n, 0); tried_spines_.assign(n, 0);
        declined_predictive_ = std::make_unique<std::atomic_bool[]>(n);
        declined_exact_ = std::make_unique<std::atomic_bool[]>(n);
        declined_spines_ = std::make_unique<std::atomic_bool[]>(n);
        for (NodeId id = 0; id < n; ++id) {
            declined_predictive_[id].store(false, std::memory_order_relaxed);
            declined_exact_[id].store(false, std::memory_order_relaxed);
            declined_spines_[id].store(false, std::memory_order_relaxed);
        }
        valid_ = true;
    }
    void ensure() const {
        if (initialization_complete_.load(std::memory_order_acquire)) return;
        std::call_once(initialized_, [&] {
            try { initialize(); }
            catch (const std::bad_alloc&) { limited_ = true; valid_ = false; }
            initialization_complete_.store(true, std::memory_order_release);
        });
    }
public:
    RegionalPredictiveCache(const Ast& ast, const OccurrenceAnalysis& analysis,
                            const std::vector<NodeId>& control,
                            const std::vector<NodeId>& equivalent_control)
        : ast_(ast), analysis_(analysis), control_(control), equivalent_control_(equivalent_control) {}

    [[nodiscard]] bool reachable(NodeId node) const {
        ensure(); return valid_ && node < reachable_.size() && reachable_[node];
    }
    [[nodiscard]] const RegionalConsumerContract* contract(NodeId node) const {
        return reachable(node) ? &contracts_[node] : nullptr;
    }
    [[nodiscard]] std::size_t contract_id(NodeId node) const {
        return reachable(node) ? contract_ids_[node] : no_id;
    }
    [[nodiscard]] std::size_t analysis_steps() const { ensure(); return work_; }
    [[nodiscard]] bool limit_reached() const { ensure(); return limited_; }
    [[nodiscard]] RegionalPreparationStatistics statistics() const {
        // Observing an unused cache must not prepare it, particularly for Auto
        // routes that already bypass native regional execution entirely.
        if (!initialization_complete_.load(std::memory_order_acquire)) return {};
        std::lock_guard lock(mutex_);
        auto result = statistics_; result.follow_steps = work_; return result;
    }

    [[nodiscard]] std::shared_ptr<const PredictiveRegion> certificate(NodeId node, bool exact = false) const {
        if (!reachable(node)) return {};
        auto& declined = exact ? declined_exact_[node] : declined_predictive_[node];
        if (declined.load(std::memory_order_acquire)) return {};
        std::lock_guard lock(mutex_);
        auto& cached = exact ? exact_[node] : predictive_[node];
        auto& tried = exact ? tried_exact_[node] : tried_predictive_[node];
        if (!tried) {
            tried = 1;
            const auto& c = contracts_[node];
            if ((exact || c.eof || c.bytes.any()) && admit_preparation()) {
                ++statistics_.predictive_builds;
                try {
                    auto prepared = std::make_shared<PredictiveRegion>(ast_, analysis_,
                        PredictiveRegion::Scope{node, exact ? std::bitset<256>{} : c.bytes, exact || c.eof});
                    statistics_.analysis_steps += prepared->analysis_steps;
                    if (prepared->certified) {
                        const auto bytes = sizeof(PredictiveRegion) +
                            prepared->targets.capacity() * sizeof(NodeId) +
                            prepared->choices.capacity() * sizeof(PredictiveRegion::Choice) +
                            prepared->choice_index.capacity() * sizeof(std::size_t);
                        if (retain(bytes)) cached = std::move(prepared);
                    } else {
                        ++statistics_.declined; statistics_.limit_declines += prepared->limit_reached;
                    }
                } catch (const std::bad_alloc&) {
                    ++statistics_.allocation_declines; ++statistics_.declined;
                }
            }
        }
        // tried is permanent: a null result cannot later acquire a positive
        // certificate. Publish this fact only after its preparation completes.
        if (!cached) declined.store(true, std::memory_order_release);
        return cached;
    }
    [[nodiscard]] std::shared_ptr<const ExactEndSpines> spine(NodeId node) const {
        if (!reachable(node)) return {};
        if (declined_spines_[node].load(std::memory_order_acquire)) return {};
        std::lock_guard lock(mutex_);
        if (!tried_spines_[node]) {
            tried_spines_[node] = 1;
            if (admit_preparation()) {
                ++statistics_.spine_builds;
                try {
                    auto prepared = std::make_shared<ExactEndSpines>(ast_, analysis_, node);
                    statistics_.analysis_steps += prepared->analysis_steps;
                    if (prepared->certified) {
                        auto bytes = sizeof(ExactEndSpines) + prepared->states.capacity() * sizeof(ExactEndSpines::State);
                        for (const auto& state : prepared->states) {
                            bytes += state.clauses.capacity() * sizeof(ExactEndSpines::Clause);
                            for (const auto& clause : state.clauses)
                                bytes += (clause.prefix.capacity() + clause.suffix.capacity()) * sizeof(NodeId);
                        }
                        if (retain(bytes)) spines_[node] = std::move(prepared);
                    } else {
                        ++statistics_.declined; statistics_.limit_declines += prepared->limit_reached;
                    }
                } catch (const std::bad_alloc&) {
                    ++statistics_.allocation_declines; ++statistics_.declined;
                }
            }
        }
        if (!spines_[node]) declined_spines_[node].store(true, std::memory_order_release);
        return spines_[node];
    }
};
} // namespace crank::detail
