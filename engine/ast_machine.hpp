#pragma once
#include "atomic_exclusions.hpp"
#include "occurrence_matcher.hpp"
#include "regional_statistics.hpp"
#include "exact_program_statistics.hpp"
#include "source_portfolio_statistics.hpp"
#include <atomic>
#include <span>

namespace crank { class OccurrenceAnalysis; }

namespace crank::detail {

struct EquivalenceStatistics;
struct RegularRegionStatistics;
class SourceViewPortfolio;

// Internal cooperative cancellation is never a negative recognition result.
struct AstExecutionCancelled {};

// Optional theorem search can be admitted after native work has already shown
// the query is nontrivial. The callback runs once on the foreground thread and
// must return promptly without waiting. Returned bits must outlive this query
// and certify the same immutable AST/input. A null return declines assistance.
struct DeferredAssistance {
    std::size_t after_steps = 512;
    void* context = nullptr;
    const AtomicExclusions* (*activate)(void*, std::size_t work_steps) noexcept = nullptr;
};

// The context-free execution mode of the occurrence machine. Requests address
// the original AST; returns resume an AST child boundary or repetition counter.
// Only input-independent matching environments may be shared in this mode.
class AstMachineProgram {
public:
    explicit AstMachineProgram(const Ast&);
    AstMachineProgram(const Ast&, SourcePortfolioLimits); // internal cap-policy controls
    [[nodiscard]] static bool eligible(const Ast&);
    // Immutable facts for the owned, checked AST. Aliasing ownership keeps
    // these facts alive even if this public program handle is destroyed.
    [[nodiscard]] std::shared_ptr<const OccurrenceAnalysis> shared_analysis() const;
    [[nodiscard]] const Ast& ast() const;
    [[nodiscard]] const std::vector<NodeId>& control_targets() const;
    [[nodiscard]] const EquivalenceStatistics& equivalence_statistics() const;
    [[nodiscard]] const RegularRegionStatistics& regular_region_statistics() const;
    [[nodiscard]] RegionalPreparationStatistics regional_preparation_statistics() const;
    [[nodiscard]] ExactProgramPreparationStatistics exact_program_preparation_statistics() const;
    [[nodiscard]] BoundedWordPreparationStatistics bounded_word_preparation_statistics() const;
    [[nodiscard]] CompiledProgramPreparationStatistics compiled_program_preparation_statistics() const;
    [[nodiscard]] std::array<NormalizedProgramPreparationStatistics, 4> normalized_program_preparation_statistics() const;
    [[nodiscard]] std::shared_ptr<const SourceViewPortfolio> shared_source_portfolio() const;
    [[nodiscard]] SourcePortfolioSnapshot source_portfolio_snapshot() const;
    // Research operation for the original pure Boolean full-span contract.
    // Admission is checked before execution; unavailable is an exception.
    [[nodiscard]] bool full_match_candidate(SourceCandidateId, std::string_view,
        OccurrenceMatchOptions = {}, OccurrenceStatistics* = nullptr,
        const std::atomic_bool* cancel = nullptr) const;
    // Match this entry against a view without changing the original (?R) target.
    [[nodiscard]] bool full_match_node(NodeId, std::string_view,
        OccurrenceMatchOptions, OccurrenceStatistics*, const std::atomic_bool* cancel = nullptr) const;
    // Boolean OR over entries sharing one view and one native memo. Seeding
    // entries never changes the original (?R) target; an empty set is false.
    [[nodiscard]] bool full_match_any(std::span<const NodeId>, std::string_view,
        OccurrenceMatchOptions, OccurrenceStatistics*, const std::atomic_bool* cancel = nullptr) const;
    [[nodiscard]] bool full_match(std::string_view,
        OccurrenceMatchOptions = {}, OccurrenceStatistics* = nullptr) const;
    // The native agenda remains authoritative and never waits for publishers.
    // Exclusions must certify this AST and this exact immutable subject. This
    // Boolean-only entry ignores capture output; observed count/reference
    // dependencies remain ineligible for shared context-free execution.
    [[nodiscard]] bool full_match_assisted(std::string_view,
        OccurrenceMatchOptions, const AtomicExclusions&,
        OccurrenceStatistics* = nullptr, NativeAssistanceStatistics* = nullptr) const;
    [[nodiscard]] bool full_match_deferred(std::string_view,
        OccurrenceMatchOptions, const DeferredAssistance&,
        OccurrenceStatistics* = nullptr, NativeAssistanceStatistics* = nullptr) const;
    [[nodiscard]] std::vector<std::size_t> endpoints(std::string_view,
        std::size_t start = 0, OccurrenceMatchOptions = {},
        OccurrenceStatistics* = nullptr) const;
    [[nodiscard]] std::optional<Outcome> first_match(std::string_view,
        OccurrenceMatchOptions = {}, OccurrenceStatistics* = nullptr) const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

} // namespace crank::detail
