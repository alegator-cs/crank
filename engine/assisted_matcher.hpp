#pragma once

#include "ast_machine.hpp"
#include "atomic_exclusions.hpp"
#include <string>

namespace crank {

enum class AssistancePolicy { Selective, Always, Progress };

struct AssistanceOptions {
    // Includes the caller. Background threads are constructed with the program;
    // a query never waits for a worker to become available.
    std::size_t workers = 2;
    std::size_t min_input_bytes = 1024;
    std::size_t max_input_bytes = 64 * 1024 * 1024;
    // Independent extra-work allowance, divided across admitted helpers. Native
    // max_states/max_steps are never reduced to pay for optional speculation.
    std::size_t max_helper_steps = 1000000;
    // Selective suppresses provably useless helper kinds and backs off when
    // recent queries in the same size class consumed no published proof.
    // Progress uses the same static gates but launches only after current
    // native work reaches min_native_steps, with no cross-query learning.
    // Always retains unconditional eligible-query admission as a control.
    // Default uses current-query evidence: history-only backoff can miss a
    // sudden malformed-input burst after a run of valid queries.
    AssistancePolicy policy = AssistancePolicy::Progress;
    // Progress admits at the first 64-step native poll at/after this threshold.
    // It never restarts recognition or deducts helper work from native quotas.
    std::size_t min_native_steps = 512;
};

enum class AssistanceDecision {
    Disabled, SmallInput, LargeInput, NoBudget, Unavailable,
    RootRejected, Busy, AllocationDeclined, Admitted, NoUsefulProof, PolicyBackoff,
    BelowWorkThreshold
};

struct AssistedStatistics {
    OccurrenceStatistics matching;
    detail::NativeAssistanceStatistics consumption;
    AssistanceDecision decision = AssistanceDecision::Disabled;
    std::size_t helpers_admitted = 0;
    std::size_t helpers_finished_at_return = 0;
    std::size_t copied_input_bytes = 0;
    std::size_t native_steps_before_admission = 0;
    bool root_rejected_at_return = false;
    // Native exhausted its budget, but an already-published root proof supplied
    // the final negative verdict. matching.limit_reached remains diagnostic.
    bool native_limit_rescued = false;
};

// Cumulative pool counters, sampled without waiting. Helper work/byte counters
// include completed tasks only. wait_idle() gives a complete snapshot when
// callers are no longer concurrently submitting queries.
struct AssistancePoolStatistics {
    std::size_t queries_admitted = 0;
    std::size_t queries_busy = 0;
    std::size_t queries_policy_skipped = 0;
    std::size_t queries_no_useful_proof = 0;
    std::size_t tasks_submitted = 0;
    std::size_t tasks_finished = 0;
    std::size_t tasks_cancelled = 0;
    std::size_t tasks_declined = 0;
    std::size_t helper_steps = 0;
    std::size_t helper_bytes = 0;
    std::size_t nodes_excluded = 0;
    std::size_t root_rejections = 0;
    std::size_t active_helpers = 0;
    std::uint64_t max_stop_observation_ns = 0;
    // From culler's stop observation through its private buffer destruction;
    // excludes subsequent shared job/input reclamation and pool bookkeeping.
    std::uint64_t max_cleanup_ns = 0;
};

// Pure Boolean matching through the existing native top-down algorithm.
// Helpers own their work and publish only irreversible impossibility bits.
// No foreground graph is shared, and full_match never joins helpers. The
// string_view overload copies eligible assisted inputs for safe helper lifetime;
// the owned overload retains the supplied immutable buffer instead.
class AssistedProgram {
public:
    explicit AssistedProgram(const Ast&, AssistanceOptions = {});
    [[nodiscard]] static bool eligible(const Ast&);
    [[nodiscard]] bool full_match(std::string_view,
        OccurrenceMatchOptions = {}, AssistedStatistics* = nullptr) const;
    [[nodiscard]] bool full_match(std::shared_ptr<const std::string>,
        OccurrenceMatchOptions = {}, AssistedStatistics* = nullptr) const;
    [[nodiscard]] AssistancePoolStatistics pool_statistics() const;
    // Explicit maintenance/measurement barrier; never called by full_match.
    void wait_idle() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace crank
