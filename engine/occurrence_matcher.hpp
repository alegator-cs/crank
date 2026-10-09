#pragma once
#include "source_portfolio_statistics.hpp"
#include "occurrence_path.hpp"
#include "state_matcher.hpp"
#include <memory>

namespace crank {

struct OccurrenceMatchOptions {
    // Cumulative allocation/work budgets per native attempt. Optional native
    // acceleration that exhausts retries ordinary execution with a fresh full
    // allowance; discarded charges are separately exposed in statistics.
    // State units include configurations,
    // continuation frames and copied scalar environment slots. Occurrence units
    // include records copied when a shared environment is changed or returned.
    std::size_t max_states = 1000000;
    std::size_t max_steps = 4000000;
    std::size_t max_occurrences = 4000000;
    std::size_t max_repeat = 10000;
    // Number of simultaneously active source-rule invocations, not C++ depth.
    std::size_t max_call_depth = 1024;
    // Optional arithmetic certificates and pure Boolean/endpoint byte-boundary
    // and ordered-pair checks. Remaining-input feasibility checks stay enabled: they also bound
    // productive left-recursive descent. Witness/history byte checks are absent.
    bool prune = true;
    // Share context-free AST requests and return links for Boolean, endpoint,
    // and first-witness queries. Disable to compare the original history VM.
    // This mode uses max_states/max_steps; history/depth/repeat caps apply only
    // to the history VM. Traces and all outcomes always use the history VM.
    bool share_calls = true;
    // Pure Boolean/endpoint queries may freeze subscribers by input position
    // and summarize unique transparent return chains. Witness/history queries
    // retain their original scheduler and proof/capture behavior.
    bool compress_returns = true;
    // Skip language-preserving Capture/Call and unary control wrappers when
    // capture effects are not observed. The compiled AST itself stays intact.
    bool collapse_control = true;
    // Merge equivalent caller completions and small concatenation/repetition
    // continuations in the ordered native scheduler. Frozen return lists use
    // word masks to skip covered fanout. Witness/history paths are unchanged.
    bool coalesce_returns = true;
    // A write-only nullable body's scalar transition relation stabilizes above
    // remaining bytes + twice the live capture slots + one. Clamp repetition
    // bounds there when the iteration count/history has no observer. This is
    // a VM optimization only; disable for same-binary differential checks.
    bool bound_scalar_repetitions = true;
    // Pure ordered terminal consumers can resume directly without allocating
    // a one-byte request cell and completion chain. Witnesses are unchanged.
    bool inline_terminals = true;
    // Compile-time structural equivalence can share pure language nodes while
    // preserving the original AST and every observed capture/history path.
    bool merge_equivalent_nodes = true;
    // Ordered queries intern only at the current frontier; stable graph IDs
    // survive while obsolete lookup indices are discarded after closure.
    bool frontier_indexes = true;
    // Once an internal request's start frontier closes, no new subscriber can
    // replay its history. Keep its latest ordered result for deduplication;
    // accepting endpoint exports and witness/unordered execution remain intact.
    bool compact_closed_results = true;
    // Pure dense transparent consumers share caller-word sets and agenda deltas.
    bool aggregate_returns = true;
    // Correlated caller sets with the same remaining concat language share
    // child demands; distinct exact-end demands are never merged.
    bool aggregate_suffix_returns = true;
    // Frozen equal-mask caller-word runs share endpoint coverage certificates.
    bool aggregate_interval_runs = true;
    // A fixed masked first word plus a uniform interior has prefix-union form.
    bool aggregate_prefix_blocks = true;
    // Exact acyclic regions inside recursive grammars may use a DFA cursor on
    // the same native ordered agenda, publishing ordinary endpoint results.
    bool local_regular_regions = true;
    // Suppress local DFA endpoints unusable by every frozen consumer.
    bool endpoint_consumer_guards = true;
    // Bound unproductive probes in a fixed source/next-byte admission table.
    bool endpoint_consumer_admission = true;
    // Reuse one deterministic cursor across ordered byte frontiers. Disabling
    // this restores one ordinary agenda task per DFA transition for ablation.
    bool regular_cursor_frontiers = true;
    // Each request's one downward action is implicit in the unexpanded suffix
    // of the request arena. Task watermarks preserve exact FIFO interleaving.
    bool implicit_request_agenda = true;
    // A nullable node whose FIRST set excludes this frontier byte has exactly
    // the epsilon endpoint here; resume the pure consumer without a request.
    bool inline_nullable_frontiers = true;
    // Closed pure Boolean recursion with a bounded FIRST/FOLLOW certificate
    // executes the same AST with a compact continuation stack. Other query
    // contracts retain native request/return recognition.
    bool predictive_recursion = true;
    // Lazy closed-root word refinement follows the existing cheap predictors.
    bool bounded_word_prediction = true;
    // A bounded conflict-free source SLR program delays source alternatives
    // until their discriminator, using one deterministic stack at runtime.
    bool deterministic_programs = true;
    // Lower already-certified root Boolean machines to immutable executable
    // layouts. Disable all or one family for same-program differential checks.
    // These switches change execution, never source-language admission.
    bool compiled_exact_programs = true;
    bool lowered_slr = true;
    bool lowered_predictive = true;
    bool lowered_spines = true;
    // Reuse a certified repeat guard as the following terminal check while
    // retaining the original tick/frame-charge sequence.
    bool predictive_observation_reuse = true;
    // Optional host-native control for an already-certified SLR program.
    // Unsupported hosts and failed native preparation retain portable code.
    bool native_slr = false;
    // Bounded pure-Boolean source views. Cheap original candidates retain
    // precedence; quotients can expose existing predictors before SLR.
    bool normalized_exact_programs = true;
    bool normalize_repetition_partitions = true;
    bool normalize_transparent_calls = true;
    // Five-helper chain law for the narrowly matched ambiguous binary grammar.
    // Only full-root Boolean queries may consume its ordinary predictor.
    bool normalize_arithmetic_chains = true;
    // Equal pure continuation frames may be stored as a run. Terminal edges
    // need no temporary frame; repeated terminal exits execute in byte batches.
    bool predictive_continuation_runs = true;
    // A pure full-root Boolean demand can consume certified fixed-width rims
    // from both ends around a single recursive child. The remaining span and
    // rule are the entire recursive state; endpoint/witness queries do not use it.
    bool exact_end_spines = true;
    // A fixed trailing word can be checked from the demanded end before a
    // residual root receives its own FIRST/FOLLOW certificate and EOF boundary.
    bool predictive_exact_rims = true;
    // Scoped exact regional machines execute only in pure, ordered full-root
    // Boolean recognition. Completed answers wait for their legal frontier.
    bool regional_execution = true;
    // Derive closed root FOLLOW contracts before replacing prefix producers.
    bool regional_consumers = true;
    // Bounded local boundary deductions may create exact regional demands.
    bool regional_constraints = true;
    bool regional_length_facts = true;
    bool regional_byte_facts = true;
};

struct OccurrenceStatistics {
    std::size_t states = 0;
    std::size_t work_steps = 0;
    std::size_t down_steps = 0;
    std::size_t up_steps = 0;
    std::size_t pruned = 0;
    std::size_t arithmetic_pruned = 0;
    std::size_t path_checks = 0;
    std::size_t forward_obligations = 0;
    std::size_t max_call_depth = 0;
    std::size_t occurrences = 0;
    std::size_t memo_entries = 0;
    std::size_t memo_hits = 0;
    std::size_t return_links = 0;
    std::size_t completions = 0;
    std::size_t replay_steps = 0;
    std::size_t proof_nodes = 0;
    std::size_t max_pending = 0;
    std::size_t compression_lookups = 0;
    std::size_t compression_hops = 0;
    std::size_t compressed_completions = 0;
    std::size_t compression_cycles = 0;
    std::size_t control_redirects = 0;
    std::size_t ordered_positions = 0;
    std::size_t duplicate_completions = 0;
    std::size_t coalesced_returns = 0;
    std::size_t ordered_endpoint_checks = 0;
    std::size_t fanout_word_checks = 0;
    std::size_t fanout_links_skipped = 0;
    std::size_t scalar_repetition_clamps = 0;
    std::size_t scalar_iteration_bound_reduction = 0;
    std::size_t inline_terminal_transitions = 0;
    std::size_t aggregate_return_links = 0;
    std::size_t aggregate_return_words = 0;
    std::size_t aggregate_word_tasks = 0;
    std::size_t aggregate_completion_members = 0;
    std::size_t aggregate_word_checks = 0;
    std::size_t aggregate_run_build_words = 0;
    std::size_t aggregate_runs = 0;
    std::size_t aggregate_run_checks = 0;
    std::size_t aggregate_run_words_skipped = 0;
    std::size_t aggregate_run_cache_entries = 0;
    std::size_t aggregate_prefix_blocks = 0;
    std::size_t aggregate_prefix_checks = 0;
    std::size_t aggregate_prefix_words_skipped = 0;
    std::size_t aggregate_prefix_cache_entries = 0;
    std::size_t suffix_word_tasks = 0;
    std::size_t suffix_members = 0;
    std::size_t native_agenda_tasks = 0;
    // Physical result slots, distinct from logical successful completions.
    std::size_t native_result_slots = 0;
    std::size_t native_reused_result_slots = 0;
    std::size_t implicit_down_steps = 0;
    std::size_t inline_nullable_transitions = 0;
    std::size_t spine_attempts = 0;
    std::size_t spine_steps = 0;
    std::size_t spine_transitions = 0;
    std::size_t spine_boundary_bytes = 0;
    std::size_t max_spine_state_slots = 0;
    std::size_t native_spine_fallbacks = 0;
    std::size_t predictive_rim_attempts = 0;
    std::size_t predictive_rim_bytes = 0;
    std::size_t native_predictive_rim_fallbacks = 0;
    std::size_t bounded_word_attempts = 0;
    std::size_t bounded_word_steps = 0;
    std::size_t bounded_word_frames = 0;
    std::size_t bounded_word_max_frames = 0;
    std::size_t native_bounded_word_fallbacks = 0;
    std::size_t exact_program_attempts = 0;
    std::size_t exact_program_actions = 0;
    std::size_t exact_program_shifts = 0;
    std::size_t exact_program_reductions = 0;
    std::size_t exact_program_max_stack = 0;
    std::size_t native_exact_program_fallbacks = 0;
    std::size_t lowered_slr_attempts = 0;
    std::size_t lowered_predictive_attempts = 0;
    std::size_t lowered_bounded_word_attempts = 0;
    std::size_t lowered_spine_attempts = 0;
    std::size_t native_slr_attempts = 0;
    std::size_t normalized_exact_attempts = 0;
    std::size_t selected_source_view = no_id, selected_source_representation = no_id;
    std::size_t source_view_regular_steps = 0;
    std::size_t native_normalized_program_fallbacks = 0;
    std::size_t predictive_attempts = 0;
    std::size_t predictive_steps = 0;
    std::size_t predictive_frames = 0;
    std::size_t max_predictive_frames = 0;
    std::size_t native_predictive_fallbacks = 0;
    std::size_t predictive_frame_runs = 0;
    std::size_t max_predictive_frame_runs = 0;
    std::size_t predictive_terminal_run_bytes = 0;
    std::size_t regional_attempts = 0;
    std::size_t regional_successes = 0;
    std::size_t regional_failures = 0;
    std::size_t regional_declines = 0;
    std::size_t regional_steps = 0;
    std::size_t regional_certificate_uses = 0;
    std::size_t regional_spine_attempts = 0;
    std::size_t regional_predictive_attempts = 0;
    std::size_t regional_deferred_completions = 0;
    std::size_t regional_published_completions = 0;
    std::size_t max_regional_pending = 0;
    std::size_t regional_frontier_jumps = 0;
    std::size_t regional_constraint_probes = 0;
    std::size_t regional_constraint_refinements = 0;
    std::size_t regional_constraint_impossible = 0;
    std::size_t regional_constraint_steps = 0;
    std::size_t regional_constraint_plan_builds = 0;
    std::size_t regional_constraint_plan_steps = 0;
    std::size_t regional_constraint_visits = 0;
    std::size_t regional_constraint_capped = 0;
    std::size_t max_regional_boundary_domain = 0;
    std::size_t native_regional_fallbacks = 0;
    std::size_t regional_discarded_states = 0;
    std::size_t regional_discarded_steps = 0;
    std::size_t suffix_groups_created = 0;
    std::size_t regular_region_requests = 0;
    std::size_t regular_region_transitions = 0;
    std::size_t regular_region_cursors = 0;
    std::size_t max_regular_cursors = 0;
    std::size_t regular_region_endpoints = 0;
    std::size_t endpoint_consumer_checks = 0;
    std::size_t endpoint_consumer_rejections = 0;
    std::size_t endpoint_consumer_declines = 0;
    // Primary counters describe the final attempt. Add discarded charges to
    // states/work_steps when measuring total work, including a failed attempt.
    // Retry tiers: new frontier representations, then older local/aggregate
    // summaries. Each tier is disabled monotonically and retried at most once.
    std::size_t native_acceleration_fallbacks = 0;
    std::size_t native_consumer_fallbacks = 0;
    std::size_t native_frontier_fallbacks = 0;
    std::size_t native_ordinary_fallbacks = 0;
    std::size_t native_acceleration_discarded_states = 0;
    std::size_t native_acceleration_discarded_steps = 0;
    bool limit_reached = false;
};

enum class OccurrenceKind { Root, Group, Count, Iteration };

struct OccurrenceRecord {
    std::size_t id = no_id;
    std::size_t parent = no_id;
    OccurrenceKind kind = OccurrenceKind::Root;
    // Group: capture slot. Count/Iteration: count slot. Root: zero.
    std::size_t source_id = no_id;
    NodeId source_node = no_id;
    std::string name;
    Span span;
    std::optional<std::size_t> count;
    std::optional<std::size_t> iteration;
    std::size_t depth = 0;
    std::size_t height = 0;
    bool source_rule = false;
    bool closed = false;
    std::vector<std::size_t> children;
};

struct TracedOutcome {
    Outcome outcome;
    // Preorder allocation IDs, with the synthetic whole-match scope at zero.
    // Only this successful derivation is present; alternative histories are
    // never combined merely because their byte spans agree.
    std::vector<OccurrenceRecord> occurrences;
};

// Resolve a path against one completed trace. Relative paths use scope as the
// current occurrence ID; missing/mismatched selectors return an empty optional.
[[nodiscard]] std::optional<std::size_t> resolve_occurrence_value(
    const TracedOutcome&, const CountPath&, std::size_t scope = 0);

// A fair heap-resident continuation machine for recursive, stateful matching.
// Backreferences/forward references and bare count uses retain global latest
// binding semantics. Explicit paths read occurrence-local state. Rule depth is
// zero at the first source invocation. Explicit calls and inline captures that
// are call targets add rule frames; other captures and repetition scopes do not.
// The initial whole-pattern scope is a rule frame only when (?R) calls it.
// Height counts source-rule child edges; a leaf invocation has height zero.
// Forward paths freeze their anchor and each component as it becomes available;
// an absent unindexed component chooses its next occurrence. A closed scope
// with a missing child is a mismatch. Depth is available on entry; byte size,
// height and count become available on completion. Guessed forward values are
// accepted only after the completed occurrence proves every obligation.
//
// Branches exceeding repeat/depth budgets are deferred: a witnessed Boolean or
// first-match result remains valid. A negative answer or exhaustive enumeration
// is never returned if a deferred branch could still produce another result.
class OccurrenceProgram {
public:
    explicit OccurrenceProgram(const Ast&);
    // Structural certificate: no executed runtime references or count effects.
    [[nodiscard]] bool supports_shared_calls() const;
    [[nodiscard]] detail::SourcePortfolioSnapshot source_portfolio_snapshot() const;
    [[nodiscard]] bool full_match(std::string_view,
        OccurrenceMatchOptions = {}, OccurrenceStatistics* = nullptr) const;
    // General scalar facade entrypoints retain absolute subject coordinates.
    [[nodiscard]] bool exists_from(std::string_view, std::size_t start, bool full,
        OccurrenceMatchOptions = {}, OccurrenceStatistics* = nullptr) const;
    [[nodiscard]] std::optional<Outcome> first_from(std::string_view,
        std::size_t start, bool full, OccurrenceMatchOptions = {},
        OccurrenceStatistics* = nullptr) const;
    [[nodiscard]] std::vector<std::size_t> endpoints(std::string_view,
        std::size_t start = 0, OccurrenceMatchOptions = {},
        OccurrenceStatistics* = nullptr) const;
    [[nodiscard]] std::optional<Outcome> first_match(std::string_view,
        OccurrenceMatchOptions = {}, OccurrenceStatistics* = nullptr) const;
    [[nodiscard]] std::vector<Outcome> match_outcomes(std::string_view,
        std::size_t start = 0, bool full = true,
        OccurrenceMatchOptions = {}, OccurrenceStatistics* = nullptr) const;
    [[nodiscard]] std::optional<TracedOutcome> first_traced_match(std::string_view,
        OccurrenceMatchOptions = {}, OccurrenceStatistics* = nullptr) const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace crank
