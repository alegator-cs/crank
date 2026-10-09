#pragma once
#include "boolean_source_view.hpp"
#include "exact_lowering_cache.hpp"
#include "predictive_regions.hpp"
#include "bounded_word_regions.hpp"
#include "deterministic_program.hpp"
#include "exact_end_spines.hpp"
#include "regular_matcher.hpp"
#include "lowered_predictive_program.hpp"
#include "lowered_deterministic_program.hpp"
#include "lowered_spine_program.hpp"
#include "native_slr_program.hpp"
#include <mutex>
#include <tuple>

namespace crank::detail {

// One typed certificate on one immutable source view. Lowerings are lazy and
// are destroyed before their certificate; the enclosing owner destroys this
// candidate before the source graph and fresh facts it borrows.
class SourceViewCandidate {
    friend class SourceViewPortfolio;
    const BooleanSourceView& source_;
    SourceCandidateStatistics statistics_;
    std::unique_ptr<RegularProgram> regular_;
    std::unique_ptr<PredictiveRegion> predictive_;
    std::unique_ptr<BoundedWordRegion> bounded_word_;
    std::unique_ptr<DeterministicProgram> slr_;
    std::unique_ptr<ExactEndSpines> inward_;
    SourceViewCandidate(const BooleanSourceView& source, SourceCandidateId id) : source_(source) {
        statistics_.id = id;
        const auto& ast = source.ast(); const auto& facts = source.analysis();
        bool admitted = false, limited = false;
        try {
            switch (id.representation) {
            case SourceRepresentation::Regular:
                if (!RegularProgram::eligible(ast)) {
                    statistics_.status = SourcePreparationStatus::Inapplicable; return;
                }
                regular_ = std::make_unique<RegularProgram>(ast);
                statistics_.states = regular_->construction_statistics().dfa_states;
                statistics_.retained_bytes = regular_->retained_bytes();
                admitted = true; break;
            case SourceRepresentation::Predictive:
                predictive_ = std::make_unique<PredictiveRegion>(ast, facts);
                admitted = predictive_->certified; limited = predictive_->limit_reached;
                statistics_.analysis_steps = predictive_->analysis_steps;
                statistics_.decision_rows = predictive_->choices.size();
                statistics_.decline_node = predictive_->decline_node;
                statistics_.decline_reason = predictive_->decline_reason;
                statistics_.retained_bytes = sizeof(PredictiveRegion) + predictive_->targets.capacity() * sizeof(NodeId) +
                    predictive_->choices.capacity() * sizeof(PredictiveRegion::Choice) +
                    predictive_->choice_index.capacity() * sizeof(std::size_t);
                break;
            case SourceRepresentation::BoundedWord:
                bounded_word_ = std::make_unique<BoundedWordRegion>(ast, facts);
                admitted = bounded_word_->certified; limited = bounded_word_->limit_reached;
                statistics_.analysis_steps = bounded_word_->analysis_steps;
                statistics_.decision_rows = bounded_word_->decision_rows;
                statistics_.retained_bytes = bounded_word_->retained_bytes();
                break;
            case SourceRepresentation::Slr:
                slr_ = std::make_unique<DeterministicProgram>(ast, facts);
                admitted = slr_->certified(); limited = slr_->statistics().limited;
                statistics_.analysis_steps = slr_->statistics().analysis_steps;
                statistics_.retained_bytes = slr_->statistics().retained_bytes;
                statistics_.states = slr_->statistics().states;
                statistics_.productions = slr_->statistics().productions;
                statistics_.conflicts = slr_->statistics().conflicts;
                statistics_.conflict_state = slr_->statistics().conflict_state;
                statistics_.conflict_token = slr_->statistics().conflict_token;
                statistics_.conflict_existing_action = slr_->statistics().conflict_existing_action;
                statistics_.conflict_new_action = slr_->statistics().conflict_new_action;
                if (statistics_.conflicts) statistics_.decline_reason = "SLR action conflict";
                statistics_.normalized_alias_calls = slr_->statistics().normalized_alias_calls;
                break;
            case SourceRepresentation::Inward:
                inward_ = std::make_unique<ExactEndSpines>(ast, facts);
                admitted = inward_->certified; limited = inward_->limit_reached;
                statistics_.analysis_steps = inward_->analysis_steps;
                statistics_.states = inward_->states.size();
                statistics_.retained_bytes = sizeof(ExactEndSpines) + inward_->states.capacity() * sizeof(ExactEndSpines::State);
                for (const auto& state : inward_->states) {
                    statistics_.retained_bytes += state.clauses.capacity() * sizeof(ExactEndSpines::Clause);
                    for (const auto& clause : state.clauses)
                        statistics_.retained_bytes += (clause.prefix.capacity() + clause.suffix.capacity()) * sizeof(NodeId);
                }
                break;
            }
            statistics_.status = admitted ? SourcePreparationStatus::Ready :
                limited ? SourcePreparationStatus::CertificateLimit : SourcePreparationStatus::AdmissionDeclined;
            if (admitted) statistics_.retained_bytes += sizeof(*this);
        } catch (const std::bad_alloc&) { statistics_.status = SourcePreparationStatus::AllocationFailure; }
          catch (const std::length_error&) { statistics_.status = SourcePreparationStatus::CertificateLimit; }
          catch (const std::invalid_argument&) { statistics_.status = SourcePreparationStatus::Inapplicable; }
    }
public:
    mutable ExactLoweringCache<LoweredPredictiveProgram> lowered_predictive;
    mutable ExactLoweringCache<LoweredBoundedWordProgram> lowered_bounded_word;
    mutable ExactLoweringCache<LoweredDeterministicProgram> lowered_slr;
    mutable ExactLoweringCache<LoweredSpineProgram> lowered_inward;
    mutable ExactLoweringCache<NativeSlrProgram> native_slr;
    [[nodiscard]] bool ready() const { return statistics_.status == SourcePreparationStatus::Ready; }
    [[nodiscard]] const SourceCandidateStatistics& statistics() const { return statistics_; }
    [[nodiscard]] const BooleanSourceView& source_view() const { return source_; }
    [[nodiscard]] const RegularProgram* regular() const { return regular_.get(); }
    [[nodiscard]] const PredictiveRegion* predictive() const { return predictive_.get(); }
    [[nodiscard]] const BoundedWordRegion* bounded_word() const { return bounded_word_.get(); }
    [[nodiscard]] const DeterministicProgram* slr() const { return slr_.get(); }
    [[nodiscard]] const ExactEndSpines* inward() const { return inward_.get(); }
};

// This small preparation facility is intentionally root-only and source-only.
// It neither selects a public observation nor runs Native. The caller proves
// pure, unassisted, original-root full-span Boolean scope before execution.
// An owner has one immutable limits configuration. Semantic negatives and cap
// failures are cached for that owner; economic skips are never cached failures.
class SourceViewPortfolio {
    std::shared_ptr<const Ast> owned_original_;
    const Ast& original_;
    SourcePortfolioLimits limits_;
    mutable std::mutex mutex_;
    mutable SourcePortfolioSnapshot statistics_;
    mutable std::array<std::unique_ptr<BooleanSourceView>, SourcePortfolioSnapshot::view_count> views_;
    mutable std::array<std::array<std::unique_ptr<SourceViewCandidate>, 5>, SourcePortfolioSnapshot::view_count> candidates_;
    static void check(SourceCandidateId id) {
        if (id.view >= SourcePortfolioSnapshot::view_count || static_cast<unsigned>(id.representation) >= 5 ||
            (id.view == SourcePortfolioSnapshot::arithmetic_chain_view && id.representation != SourceRepresentation::Predictive))
            throw std::invalid_argument("invalid source-view candidate identity");
    }
    static std::size_t work_bound(SourceRepresentation kind) {
        switch (kind) {
        case SourceRepresentation::Regular: return 1048576;
        case SourceRepresentation::Predictive: return 1048576;
        case SourceRepresentation::BoundedWord: return 131072;
        case SourceRepresentation::Slr: return 1048576;
        case SourceRepresentation::Inward: return 262144;
        }
        return 0;
    }
    const BooleanSourceView* prepare_view_locked(unsigned view) const {
        auto& record = statistics_.views[view].preparation;
        if (record.attempted) return views_[view] && views_[view]->ready() ? views_[view].get() : nullptr;
        record.attempted = true;
        const auto reject = [&] { record.status = SourcePreparationStatus::PortfolioLimit; record.limited = true; record.retained_bytes = 0; };
        if (statistics_.view_attempts >= limits_.max_views ||
            original_.nodes.size() > limits_.max_transformed_nodes - statistics_.transformed_nodes ||
            statistics_.analysis_steps >= limits_.max_analysis_steps) { reject(); return nullptr; }
        ++statistics_.view_attempts;
        try {
            auto value = std::make_unique<BooleanSourceView>(original_,
                BooleanSourceView::Options{bool(view & 1U), bool(view & 2U), false,
                    view == SourcePortfolioSnapshot::arithmetic_chain_view},
                limits_.max_analysis_steps - statistics_.analysis_steps);
            record = value->statistics(); statistics_.analysis_steps += value->compilation_steps();
            if (!value->ready()) { record.retained_bytes = 0; return nullptr; }
            if (value->ast().nodes.size() > limits_.max_transformed_nodes - statistics_.transformed_nodes ||
                value->retained_bytes() > limits_.max_retained_bytes - statistics_.retained_bytes) { reject(); return nullptr; }
            statistics_.transformed_nodes += value->ast().nodes.size();
            statistics_.retained_bytes += value->retained_bytes();
            views_[view] = std::move(value);
        } catch (const std::bad_alloc&) { record.status = SourcePreparationStatus::AllocationFailure; }
        return views_[view] && views_[view]->ready() ? views_[view].get() : nullptr;
    }
public:
    struct BorrowedSource {}; // only owners with an immutable, dominating AST lifetime
    explicit SourceViewPortfolio(const Ast& source, SourcePortfolioLimits limits = {})
        : SourceViewPortfolio(std::make_shared<const Ast>(source), limits) {}
    explicit SourceViewPortfolio(std::shared_ptr<const Ast> source, SourcePortfolioLimits limits = {})
        : owned_original_(std::move(source)), original_(*owned_original_), limits_(limits) { initialize(); }
    SourceViewPortfolio(const Ast& source, BorrowedSource, SourcePortfolioLimits limits = {})
        : original_(source), limits_(limits) { initialize(); }
private:
    void initialize() {
        statistics_.source_nodes = original_.nodes.size();

    }
public:
    // Research construction owns a source copy; the production borrowing form
    // is held inside Impl. Its aliasing public handle retains that full owner.
    [[nodiscard]] const SourcePortfolioLimits& limits() const { return limits_; }
    [[nodiscard]] const BooleanSourceView* prepare_view(unsigned view) const {
        if (view >= SourcePortfolioSnapshot::view_count) throw std::invalid_argument("invalid source view");
        std::lock_guard lock(mutex_); return prepare_view_locked(view);
    }
    [[nodiscard]] const BooleanSourceView* view(unsigned view) const {
        if (view >= SourcePortfolioSnapshot::view_count) throw std::invalid_argument("invalid source view");
        std::lock_guard lock(mutex_); return views_[view].get();
    }
    [[nodiscard]] SourcePortfolioSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        auto result = statistics_;
        for (unsigned view = 0; view < SourcePortfolioSnapshot::view_count; ++view) for (unsigned kind = 0; kind < 5; ++kind) {
            const auto* candidate = candidates_[view][kind].get(); if (!candidate) continue;
            auto& row = result.views[view].candidates[kind];
            row.lowered_predictive = candidate->lowered_predictive.statistics();
            row.lowered_bounded_word = candidate->lowered_bounded_word.statistics();
            row.lowered_slr = candidate->lowered_slr.statistics();
            row.lowered_inward = candidate->lowered_inward.statistics();
            row.native_slr = candidate->native_slr.statistics();
            result.lowering_retained_bytes += row.lowered_predictive.retained_bytes + row.lowered_bounded_word.retained_bytes +
                row.lowered_slr.retained_bytes + row.lowered_inward.retained_bytes + row.native_slr.retained_bytes;
        }
        return result;
    }
    [[nodiscard]] const SourceViewCandidate* prepare(SourceCandidateId id) const {
        check(id); std::lock_guard lock(mutex_);
        const auto kind = static_cast<unsigned>(id.representation);
        auto& record = statistics_.views[id.view].candidates[kind];
        if (record.status != SourcePreparationStatus::Unattempted)
            return candidates_[id.view][kind] && candidates_[id.view][kind]->ready() ? candidates_[id.view][kind].get() : nullptr;
        const auto* source = prepare_view_locked(id.view);
        if (!source) { record.status = statistics_.views[id.view].preparation.status; return nullptr; }
        const auto bound = work_bound(id.representation);
        if (statistics_.candidate_attempts >= limits_.max_candidate_attempts ||
            bound > limits_.max_candidate_work - statistics_.reserved_candidate_work) {
            record.status = SourcePreparationStatus::PortfolioLimit; return nullptr;
        }
        ++statistics_.candidate_attempts; statistics_.reserved_candidate_work += bound;
        try {
            auto candidate = std::unique_ptr<SourceViewCandidate>(new SourceViewCandidate(*source, id));
            record = candidate->statistics();
            statistics_.candidate_analysis_steps += record.analysis_steps;
            if (!candidate->ready()) { record.retained_bytes = 0; return nullptr; }
            if (record.retained_bytes > limits_.max_retained_bytes - statistics_.retained_bytes) {
                record.status = SourcePreparationStatus::PortfolioLimit; record.retained_bytes = 0; return nullptr;
            }
            statistics_.retained_bytes += record.retained_bytes;
            candidates_[id.view][kind] = std::move(candidate);
        } catch (const std::bad_alloc&) { record.status = SourcePreparationStatus::AllocationFailure; }
        return candidates_[id.view][kind] && candidates_[id.view][kind]->ready() ? candidates_[id.view][kind].get() : nullptr;
    }
    [[nodiscard]] const SourceViewCandidate* cached(SourceCandidateId id) const {
        check(id); std::lock_guard lock(mutex_);
        const auto* candidate = candidates_[id.view][static_cast<unsigned>(id.representation)].get();
        return candidate && candidate->ready() ? candidate : nullptr;
    }
    // A completed repetition pass with no rewrite leaves every node and its
    // provenance unchanged. Combined (repetition then identity) consequently
    // has exactly the identity-only graph. Skip its duplicate production work
    // without publishing a negative certificate or limiting explicit research.
    // Inapplicable, incomplete and capped passes establish no such equality.
    [[nodiscard]] bool skip_redundant_combined() const {
        std::lock_guard lock(mutex_);
        if (statistics_.views[1].preparation.status != SourcePreparationStatus::Unchanged) return false;
        statistics_.combined_skipped_as_duplicate = true;
        return true;
    }
    // One inspectable precedence policy is shared by production's lazy
    // schedule and selection among deliberately considered candidates. The
    // original strong routes win first; cheap repetition-view prediction may
    // precede original SLR; all remaining alternatives follow. This is a small
    // preparation heuristic, not a fitted estimate or an optimum guarantee.
    static constexpr std::array<SourceCandidateId, 21> default_schedule() {
        using R = SourceRepresentation;
        return {{{0,R::Regular},{0,R::Inward},{0,R::Predictive},{0,R::BoundedWord},
                 {1,R::Predictive},{1,R::BoundedWord},{4,R::Predictive},{0,R::Slr},
                 {1,R::Regular},{1,R::Inward},{1,R::Slr},
                 {2,R::Regular},{2,R::Predictive},{2,R::BoundedWord},{2,R::Inward},{2,R::Slr},
                 {3,R::Regular},{3,R::Predictive},{3,R::BoundedWord},{3,R::Inward},{3,R::Slr}}};
    }
    static constexpr unsigned candidate_bit(SourceCandidateId id) {
        return 1U << (id.view * 5U + static_cast<unsigned>(id.representation));
    }
    static constexpr unsigned default_priority(SourceCandidateId id) {
        const auto schedule = default_schedule();
        for (unsigned i = 0; i < schedule.size(); ++i) if (schedule[i] == id) return i;
        return 21;
    }
    // No preparation occurs here. An explicit considered-set prevents an
    // exhaustive diagnostic or another thread's preparation from changing the
    // normal lazy policy. The unrestricted default is useful for research.
    [[nodiscard]] std::optional<SourceCandidateId> select(unsigned allowed_views = 31,
            SourceSelectionPolicy policy = SourceSelectionPolicy::Default,
            unsigned considered_candidates = (1U << 25U) - 1U) const {
        std::lock_guard lock(mutex_);
        std::optional<SourceCandidateId> best;
        std::tuple<unsigned, unsigned> best_rank;
        const auto rank = [&](const SourceCandidateStatistics& candidate) {
            const auto id = candidate.id;
            unsigned preference = 0;
            if (policy == SourceSelectionPolicy::PreferSlr) preference = id.representation != SourceRepresentation::Slr;
            if (policy == SourceSelectionPolicy::PreferPredictor)
                preference = id.representation != SourceRepresentation::Predictive && id.representation != SourceRepresentation::BoundedWord;
            if (policy == SourceSelectionPolicy::PreferOriginal) preference = id.view != 0;
            if (policy == SourceSelectionPolicy::PreferQuotient) preference = id.view == 0;
            return std::tuple{preference, default_priority(id)};
        };
        for (const auto& row : statistics_.views) if (allowed_views & (1U << row.view))
            for (const auto& candidate : row.candidates)
                if (candidate.status == SourcePreparationStatus::Ready && (considered_candidates & candidate_bit(candidate.id))) {
                    const auto value = rank(candidate);
                    if (!best || value < best_rank) { best = candidate.id; best_rank = value; }
                }
        return best;
    }
    // Cheap structural obstruction probe, cached once. A true result means
    // only that the existing repetition law may apply, never membership.
    [[nodiscard]] bool repetition_applicable() const {
        std::lock_guard lock(mutex_);
        if (statistics_.applicability_checked) return statistics_.repetition_applicable;
        statistics_.applicability_checked = true;
        const auto n = original_.nodes.size();
        if (n > 512 || original_.root >= n) return false;
        const auto transparent = [&](NodeId initial) {
            auto id = initial;
            for (std::size_t depth = 0; depth < n; ++depth) {
                if (++statistics_.applicability_steps > 131072 || id >= n) return initial;
                const auto& node = original_.nodes[id];
                if (node.kind == Kind::Call) id = node.call_target;
                else if (node.children.size() == 1 && (node.kind == Kind::Capture || node.kind == Kind::Concat || node.kind == Kind::Alt)) id = node.children.front();
                else return id;
            }
            return initial;
        };
        const auto repeat = [&](NodeId id) {
            id = transparent(id);
            return id < n && original_.nodes[id].kind == Kind::Repeat && !original_.nodes[id].upper && original_.nodes[id].lower <= 1;
        };
        for (const auto& outer : original_.nodes) {
            if (++statistics_.applicability_steps > 131072) return false;
            if (outer.kind != Kind::Repeat || outer.lower != 0 || outer.upper || outer.children.size() != 1) continue;
            const auto body = transparent(outer.children.front());
            if (body >= n) continue;
            bool found = repeat(body);
            if (original_.nodes[body].kind == Kind::Alt)
                for (const auto child : original_.nodes[body].children) {
                    if (++statistics_.applicability_steps > 131072) return false;
                    found |= repeat(child);
                }
            if (found) return statistics_.repetition_applicable = true;
        }
        return false;
    }
};
} // namespace crank::detail
