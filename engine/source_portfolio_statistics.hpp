#pragma once
#include "exact_program_statistics.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace crank::detail {
enum class SourceRepresentation : unsigned { Regular, Predictive, BoundedWord, Slr, Inward };
enum class SourcePreparationStatus : unsigned {
    Unattempted, Ready, Inapplicable, Unchanged, PreparationLimit, AllocationFailure,
    AdmissionDeclined, CertificateLimit, PortfolioLimit
};
enum class SourceSelectionPolicy : unsigned {
    Default, PreferPredictor, PreferSlr, PreferOriginal, PreferQuotient
};
struct SourceCandidateId {
    unsigned view = 0;
    SourceRepresentation representation = SourceRepresentation::Regular;
    bool operator==(const SourceCandidateId&) const = default;
};
struct SourceViewPreparationStatistics {
    bool attempted = false, limited = false, changed = false;
    bool nullable_complete = false, first_complete = false;
    bool productive_complete = false, minimum_complete = false;
    SourcePreparationStatus status = SourcePreparationStatus::Unattempted;
    std::size_t normalization_steps = 0, nullable_steps = 0;
    std::size_t repetition_rewrites = 0, redirected_calls = 0, cloned_nodes = 0;
    std::size_t arithmetic_chain_rewrites = 0;
    std::size_t source_nodes = 0, normalized_nodes = 0, retained_bytes = 0;
};
struct SourceCandidateStatistics {
    SourceCandidateId id;
    SourcePreparationStatus status = SourcePreparationStatus::Unattempted;
    std::size_t analysis_steps = 0, retained_bytes = 0;
    std::size_t states = 0, productions = 0, decision_rows = 0, conflicts = 0;
    std::size_t normalized_alias_calls = 0;
    std::size_t decline_node = std::size_t(-1), conflict_state = std::size_t(-1), conflict_token = std::size_t(-1);
    std::int32_t conflict_existing_action = 0, conflict_new_action = 0;
    const char* decline_reason = "builder declined; detailed witness unavailable";
    LoweringPreparationStatistics lowered_predictive, lowered_bounded_word, lowered_slr, lowered_inward, native_slr;
};
struct SourceViewSnapshot {
    unsigned view = 0;
    // All paths start from the original owned source; combined view applies
    // repetition then identity. Node provenance is scoped by (owner,view,id).
    unsigned parent = 0, quotient_mask = 0;
    SourceViewPreparationStatistics preparation;
    std::array<SourceCandidateStatistics, 5> candidates;
};
struct SourcePortfolioSnapshot {
    static constexpr std::size_t policy_version = 3, quotient_version = 2;
    static constexpr unsigned view_count = 5, arithmetic_chain_view = 4;
    std::array<SourceViewSnapshot, view_count> views;
    std::size_t view_attempts = 0, candidate_attempts = 0;
    std::size_t source_nodes = 0, transformed_nodes = 0, retained_bytes = 0;
    std::size_t analysis_steps = 0, candidate_analysis_steps = 0, reserved_candidate_work = 0;
    std::size_t lowering_retained_bytes = 0;
    std::size_t applicability_steps = 0;
    bool applicability_checked = false, repetition_applicable = false;
    // Economic scheduling witness only: explicit prepare(view 3) remains
    // available, and its semantic preparation status is not changed here.
    bool combined_skipped_as_duplicate = false;
    SourcePortfolioSnapshot() {
        for (unsigned view = 0; view < views.size(); ++view) {
            auto& row = views[view]; row.view = row.quotient_mask = view;
            row.parent = view == 3 ? 1 : 0;
            for (unsigned kind = 0; kind < row.candidates.size(); ++kind)
                row.candidates[kind].id = {view, static_cast<SourceRepresentation>(kind)};
        }
    }
};
struct SourcePortfolioLimits {
    // Aggregate bounds supplement unchanged individual builder bounds.
    std::size_t max_views = 4, max_transformed_nodes = 4096;
    std::size_t max_analysis_steps = 4 * 131072;
    std::size_t max_candidate_attempts = 20;
    // Deterministic preparation reservation units, not wall-time/instruction
    // counts: individual builders retain their existing state/work/depth caps.
    // The attempt cap bounds their sum even where work counters are partial.
    std::size_t max_candidate_work = 16 * 1048576;
    std::size_t max_retained_bytes = 16 * 1048576;
};
inline const char* source_representation_name(SourceRepresentation kind) {
    switch (kind) {
    case SourceRepresentation::Regular: return "regular";
    case SourceRepresentation::Predictive: return "predictive";
    case SourceRepresentation::BoundedWord: return "bounded-word";
    case SourceRepresentation::Slr: return "slr";
    case SourceRepresentation::Inward: return "inward";
    }
    return "invalid";
}
inline const char* source_preparation_status_name(SourcePreparationStatus status) {
    switch (status) {
    case SourcePreparationStatus::Unattempted: return "unattempted";
    case SourcePreparationStatus::Ready: return "ready";
    case SourcePreparationStatus::Inapplicable: return "inapplicable";
    case SourcePreparationStatus::Unchanged: return "unchanged";
    case SourcePreparationStatus::PreparationLimit: return "view-preparation-limit";
    case SourcePreparationStatus::AllocationFailure: return "allocation-failure";
    case SourcePreparationStatus::AdmissionDeclined: return "admission-declined";
    case SourcePreparationStatus::CertificateLimit: return "certificate-limit";
    case SourcePreparationStatus::PortfolioLimit: return "portfolio-limit";
    }
    return "invalid";
}
} // namespace crank::detail
