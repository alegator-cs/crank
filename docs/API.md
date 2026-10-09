# API and query contracts

Historical document references shown as plain text refer to artifacts outside this source export.

Include `crank.hpp` and construct `crank::Program` for general routing. Include component headers for repeated specialized use. Public declarations and their comments are the precise option/counter reference; the internal `detail` namespace is not a stable extension ABI.

```cpp
#include "crank.hpp"
crank::Program grammar("(?<S>a(?&S)b|)");
crank::RunStatistics stats;
bool accepted = grammar.full_match("aaabbb", {}, &stats);
auto ends = grammar.endpoints("aaabbb!", 0);
auto witness = grammar.first_match("aaabbb");
```

## Dispatch and observations

| Query / pattern | Default path | Observable result |
|---|---|---|
| Ordinary regular Boolean | Certified greedy/scans or DFA | Membership |
| Pure acyclic executed calls, Boolean | Call-transparent direct scan or bounded DFA; native if compilation declines | Membership |
| Pure acyclic executed calls, endpoints | Bounded DFA; native if compilation declines | Sorted unique ends |
| Certified shared count/equality/affine Boolean | Exact solved plan, ordered validation | Membership |
| Other nonrecursive stateful pattern | Unified occurrence VM with scalar observations | Membership or requested bindings |
| Pure recursive Boolean | Certified inward, predictive, bounded-word or deterministic programs; native demand execution when admission declines | Membership |
| Pure recursive endpoints | Native requests with unrestricted root end | Sorted unique absolute ends |
| Pure recursive first witness | Native requests with finite proof | One scalar capture outcome |
| Recursive effects / occurrence paths / bounded binders | Occurrence VM; paths retain history | Constrained result |
| `first_trace` | Occurrence VM with explicit history | One full occurrence history |
| Recursive all outcomes with explicit `Occurrences` | Occurrence VM | Exhaustive distinct public outcomes, if complete |
| Regular compilation limit under Auto | Native requests, or an available certified greedy scan | Same requested language/query |

Pure recursive `outcomes` under Auto is an error: request a witness or explicitly choose `Occurrences`. Regular DFA cannot return scalar captures merely because captures are transparent to Boolean membership. Greedy certificates are full-match-only because committing to an endpoint would lose alternatives.

`RunOptions.backend` accepts `Auto`, `Dfa`, `States`, `Greedy`, `Pcre2` and `Occurrences`. Capability checks reject incompatible query/backend pairs instead of silently changing the requested semantics. `States` retains the scalar API contract over the unified occurrence VM; it no longer selects a separate evaluator. The archived implementations and their former selectors are described in archive restoration.

`CompileOptions.regular` sets the NFA/DFA compilation caps and a transparent expansion-depth cap. `max_expansion_depth` defaults to 2048 and prevents deep acyclic call/wrapper chains from exhausting the compiler stack. When regular compilation declines, Auto retains exact native fallback; a forced `Dfa` request reports `ResourceLimit`. The corresponding CLI flags are `--max-nfa-states` and `--max-dfa-states`.

`CompileOptions.regular.paired_transitions` defaults to `true`: an admitted DFA
may also compile the composition of two byte transitions for complete Boolean
scans. `max_paired_table_bytes` defaults to 65,536 bytes and is independently
capped at that value. A failed or disabled composition keeps the original DFA.
Single-byte transition and endpoint APIs retain their original tables; short
Boolean inputs also use the original path. `RegularStatistics.dfa_table_bytes`,
`paired_table_bytes`, and `paired_analysis_steps` expose preparation/storage.
This compilation control is separate from the occurrence machine's runtime
`compiled_exact_programs` switch. Disable both for the pre-lowering control.

For complete Boolean queries, `Program` validates the requested backend before
root proofs. With `optimize=true`, existing bounds/congruences and the admitted
integer-row proof run before regular or native dispatch. Auto also uses the
regular fragment's bounded first/last-byte and ordered-pair certificate.
A proof only rejects impossibility; feasible, unknown and exhausted results
continue. `endpoints`, witnesses and histories do not use these full-span tests.

| Root control | Default | Meaning |
|---|---|---|
| `RunOptions.boundary_proofs` | `true` | Auto's complete-span regular byte/pair proof |
| `RunOptions.call_aware_scan` | `true` | Select an existing direct scanner through eligible acyclic calls |
| `RunOptions.integer_length_certificate` | `true` | Query an admitted necessary two-count row |
| `RunOptions.integer_length_max_steps` | `256` | Independent arithmetic-proof allowance |
| `CompileOptions.prepare_integer_row` | `true` | Prepare a bounded row when structurally eligible |
| `CompileOptions.regular.max_fragment_analysis_steps` | `65536` | Bound extra regular DAG summaries and period extraction; zero declines them |

Disabling call-aware scans alone retains boundary proofs; disable both controls
to reproduce the preceding called-DFA query path. Extra analysis decline leaves
the already compiled DFA available. `boundary_checks` and `boundary_rejected`
identify the guard; a guard rejection has zero regular transitions.
`integer_length_checks`, `integer_length_steps`,
`integer_length_exhaustions`, and `integer_length_rejected` identify the arithmetic
proof. `RegularStatistics.fragment_analysis_steps` records additional structural
analysis when fragment statistics are exported; a forced DFA exports its own
statistics rather than this preparation counter. The integer proof uses no
native state/work allowance. Calling `OccurrenceProgram` directly bypasses
these `Program` root proofs and retains its own native demand checks.

`RunOptions.occurrence.share_calls=false` selects continuation/history execution where the occurrence facade is used. `compress_returns`, `collapse_control`, `coalesce_returns`, `inline_terminals`, and `merge_equivalent_nodes` are pure-query native controls; witness/history scheduling retains its own proof behavior. Pure eligibility is structurally verified from actual nodes and executed effects, not trusted convenience flags.

The native relation controls are `frontier_indexes` (expire unreachable lookup keys),
`aggregate_returns` (dense transparent caller sets), `aggregate_suffix_returns`
(shared remaining-child demands), `local_regular_regions` (exact DFA regions
inside recursive recognition), and `regular_cursor_frontiers` (persistent cursors
instead of per-byte tasks). All default to true and preserve the richer witness
and VM paths. Disabling a query control retains preparation, so these are runtime
ablations, not compilation-cost ablations. Local compilation has independent
fixed construction bounds; `CompileOptions.regular` configures the top-level DFA.

The frontier controls are `compact_closed_results` (reuse unobservable internal
endpoint-history slots), `implicit_request_agenda` (encode exact FIFO Down order
in request-arena watermarks), `inline_nullable_frontiers` (directly return a
proved epsilon-only relation), `aggregate_interval_runs`, and
`aggregate_prefix_blocks` (bounded certificates for caller bits already
published at an endpoint). Each defaults to true and has a same-binary ablation.
Logical `completions` remain distinct from `native_result_slots` and
`native_reused_result_slots`. Removing implicit Down messages does not remove
Down execution; word-union certificates do remove charged recombination work.

The current native certificate controls, all in `OccurrenceMatchOptions`, are:

| Control | Default | Observation boundary |
|---|---|---|
| `predictive_recursion` | `true` | Closed full-root Boolean demands with the bounded FIRST/FOLLOW and progress certificate |
| `predictive_continuation_runs` | `true` | Fold equal predictor frames and batch eligible repeated byte effects |
| `exact_end_spines` | `true` | Fixed-span Boolean membership through separated fixed rims and at most one recursive child |
| `predictive_exact_rims` | `true` | Validate a fixed trailing root word, then predict the certified residual at its exact end |
| `bounded_word_prediction` | `true` | Lazily refine conflicting FIRST/FOLLOW tokens to width two for pure original-root Boolean demands |
| `deterministic_programs` | `true` | Lazily compile a bounded conflict-free SLR program for pure original-root Boolean demands |
| `compiled_exact_programs` | `true` | Lower admitted root Boolean machines to immutable executable layouts |
| `lowered_slr` | `true` | Enable compact SLR execution and certified stack-transform fusion |
| `lowered_predictive` | `true` | Enable compact ordinary, exact-rim, and bounded-word prediction |
| `lowered_spines` | `true` | Enable the narrow compiled inward-rim executor |
| `regional_execution` | `true` | Replace admitted internal producers in ordered full-root Boolean recognition; defer publication to the native frontier |
| `regional_constraints` | `true` | Compose bounded input-local boundary facts to create exact regional demands |
| `regional_consumers` | `true` | Derive closed source FOLLOW contracts for regional producers |
| `regional_length_facts`, `regional_byte_facts` | `true` | Independently control the two bounded boundary fact families |
| `endpoint_consumer_guards` | `true` | Omit local DFA endpoints disproved for all frozen consumers |
| `endpoint_consumer_admission` | `true` | Limit unproductive guard probes with query-local fuel |

Disabling continuation runs retains the base certified interpreter. Disable
the whole-root execution certificates and regional execution/constraints to
retain generalized native execution. They
have independent controls; `predictive_exact_rims` may run its residual
predictor even when `predictive_recursion` is disabled. The predictor does
not run for endpoint enumeration, witnesses, histories, arbitrary nonroot
entries or multi-entry OR. Its compilation bounds are fixed internal limits:
4,096 nodes, 16,384 edges, 256 choice tables and 1,048,576 propagation/wrapper
steps. Decline retains ordinary execution. Runtime frame allocations and AST
steps use the configured native allowance.

`predictive_attempts` and `predictive_steps` report use and work;
`predictive_frames`/`max_predictive_frames` report logical pushes and peak live
depth, while `predictive_frame_runs`/`max_predictive_frame_runs` report physical
run allocations and peak live runs. Updating a split top frame's control is not
a new logical push. `predictive_terminal_run_bytes` counts charged batch-byte
work; a rejecting batch may examine fewer bytes than it charged.

`exact_end_spines` retains one rule and two span positions, without recursive
return frames. Alternatives must be disjoint by certified boundary masks or
base-length conditions, and zero-width transfers must be acyclic. Its bounded
compiler permits at most 4,096 AST nodes, 128 rule states, 512 stored clauses,
16,384 stored terminal atoms, 262,144 analysis steps, and expansion depth 256.
Finite repetitions have an upper bound of at most 64. These are sufficient
conditions: declining a certificate does not reject the input language.
`spine_attempts`, `spine_steps`, `spine_transitions`, `spine_boundary_bytes`,
and `max_spine_state_slots` expose activation and charged work. The last counter
counts active structural slots, not process memory or input bytes.

`predictive_exact_rims` admits only a root concatenation with no incoming
structural or call edge. It checks a fixed word of terminal character sets
before executing the residual root, preserving original node identities.
Suffix extraction is bounded to 1,024 atoms, 65,536 work visits and depth 64;
fixed repeats are bounded by 64. The residual has the ordinary predictive
analysis limits. `predictive_rim_attempts` and `predictive_rim_bytes` distinguish
this entry from ordinary prediction; predictive counters include its interior.
Both additions are excluded from endpoint, witness, history, arbitrary nonroot
and multiple-entry queries. See the current architecture.

`bounded_word_prediction` and `deterministic_programs` run after the existing
spine, one-byte predictor, and exact trailing-rim routes. Each owns a lazy,
thread-safe positive or negative source result shared by prepared-program
copies. They are excluded from endpoint, witness, history, arbitrary nonroot,
multi-entry, and assisted queries. Disabling these options before first use
also avoids their lazy certificate construction; reading preparation statistics
does not construct a certificate.

The bounded-word certificate uses 257-token decision rows, with EOF distinct
from bytes. Only conflicting first-byte slices acquire a second-byte analysis.
Its fixed caps are 512 source nodes, 4,096 edges, eight refined first bytes,
128 choice rows, 128 second-byte rows, and 131,072 charged analysis operations.
`bounded_word_attempts`, `bounded_word_steps`, `bounded_word_frames`, and
`bounded_word_max_frames` expose activation, charged execution, cumulative
frame pushes, and peak live frames.

The deterministic compiler normalizes the pure language and partitions all
overlapping character sets into disjoint byte classes before constructing SLR
tables. Conflicts decline admission; no precedence or greediness rule resolves
them. Caps include 4,096 source nodes, 16,384 child edges, 128 nonterminals,
1,024 productions,
64-symbol right-hand sides, 512 states, 16,384 total items, and 1,048,576 charged
analysis operations. After an action conflict it may redirect pure acyclic
rule aliases and retry within the same analysis budget. This preserves the
owned source and leaves already-admitted tables unchanged. Preparation fields
`alias_normalization_attempted`, `normalized_alias_calls`, and
`original_conflicts` separate that retry from executable lowering.
`exact_program_attempts`, `exact_program_actions`,
`exact_program_shifts`, `exact_program_reductions`, and `exact_program_max_stack`
expose execution. Its state stack retains input nesting; it is not the
constant-structural-state inward machine. Every stack push consumes a cumulative
native allocation unit.

The internal `AstMachineProgram` getters `bounded_word_preparation_statistics()`
and `exact_program_preparation_statistics()` separate attempted/certified/limited
construction, source analysis work, program size, and retained bytes from
per-query execution counters. Compilation has independent fixed caps; query
`max_steps` and `max_states` govern execution. See
exact-program architecture for the
proof boundaries and report for costs.

The compiled-exact switches are independent of source admission. Disabling
`compiled_exact_programs` retains the same certificates and executes their
interpreters. Lowering is lazy, bounded, and cached with its owning immutable
source; copies share successful and declined construction. A lowering failure
retains its interpreted exact route. The new lowerings apply only to
unassisted original-root full-span Boolean queries. Regional, endpoint,
witness, nonroot, and multi-entry execution keep their existing paths.

All previous semantic step/state charges and cancellation boundaries remain.
Fused LR transformations can remove physical table lookups and stack writes
while charging each original logical action and push. They add no retry tier.
`lowered_slr_attempts`, `lowered_predictive_attempts`,
`lowered_bounded_word_attempts`, and `lowered_spine_attempts` report activation.
The internal `compiled_program_preparation_statistics()` getter reports five
independent caches (`slr`, `predictive`, `predictive_rim`, `bounded_word`,
`spine`), each with attempt/readiness, compiler version, compilation work,
retained bytes, and executable data bytes. Reading this snapshot does not
initiate compilation. These are portable C++ data programs, not emitted native
machine code. See the compiled-exact architecture.

Consumer guards do not create a new demand identity. They inspect at most 64
requests/suffix children, require frozen return lists, and conservatively admit
unknown, repeated-control, aggregate or exhausted cases. Endpoint exports
remain observers. A 64-slot table starts with 16 unsuccessful probes per slot;
an exclusion replenishes fuel. Turning admission off probes every eligible
proper-prefix endpoint, retaining the per-proof bound. Counters distinguish
`endpoint_consumer_checks`, `endpoint_consumer_rejections`, and
`endpoint_consumer_declines`. Compilation and producer DFA transitions are
unchanged; the omitted work is publication and downstream continuation work.

`Program` caches its additional occurrence evaluator after the first query that needs it. Copies share immutable compilation, and concurrent first use is synchronized once. Requesting scalar outcomes, traces, or an explicit occurrence query does not change subsequent automatic DFA dispatch. The free compatibility functions taking an `Ast` remain stateless adapters; use `Program` or `OccurrenceProgram` to retain compilation across calls.

## Component interfaces

| Header | Main entry points / role |
|---|---|
| `regex_ast.hpp`, `occurrence_path.hpp` | `parse`, immutable-use AST nodes, resolved calls, typed paths |
| `regular_matcher.hpp` | `RegularProgram`: membership, endpoints and independent-copy batches |
| `fragment_matcher.hpp`, `external_regular.hpp` | Exact scan/DFA fragments and optional-result PCRE2 adapter |
| `optimizations.hpp` | Certified `GreedyPlan`; necessary-only `LengthFilter` |
| `count_matcher.hpp`, `equality_matcher.hpp` | Optional certified single-count and copy plans |
| `affine_matcher.hpp`, `integer_row.hpp` | Exact affine/residual plans and bounded two-variable integer rows |
| `state_matcher.hpp` | Scalar compatibility functions; shared outcomes and resource-limit types |
| `occurrence_analysis.hpp` | Completed conservative facts, residue/short-length sets and narrow height certificate |
| `occurrence_matcher.hpp` | `OccurrenceProgram`; native pure queries, scalar results, traced outcomes and path resolution |
| `input_constraints.hpp` | Synchronous input-relative indexing and negative certificates |
| `assisted_matcher.hpp` | Native Boolean recognition with optional private culling and atomic exclusions |

The small evaluator in `reference/span_regex.hpp` remains a separately built correctness oracle. It is excluded from the normal executable/library and is not a runtime fallback.

Plan factories return no plan on unsupported shapes or unsafe arithmetic/compile growth. This is a decline to general matching, not a nonmatch. `ExternalRegularProgram::try_full_match` similarly returns no Boolean result on unavailability, decline, runtime error or limit; the exact native fallback must run.

## Optional atomic assistance

```cpp
#include "assisted_matcher.hpp"
crank::AssistanceOptions policy;
policy.workers = 2; // foreground plus one optional helper
crank::AssistedProgram program(crank::parse(pattern), policy);
crank::AssistedStatistics statistics;
bool accepted = program.full_match(subject, {}, &statistics);
```

`AssistedProgram` is a separate pure-CFG Boolean API. Its helpers publish only sound negative certificates; `full_match` never waits for them. Each query keeps its full native budget. Copies share a pool without serializing native queries; busy admission falls back immediately. Borrowed inputs are copied for helper lifetime, or callers may supply a retained immutable `shared_ptr<const string>`.

The default policy is `Progress`, with two total workers, a 1,024-byte minimum input, and 512 charged native steps before an admission attempt. Static grammar gates first suppress proof kinds that cannot contribute. An eligible query attempts admission at the first 64-step native poll at or beyond the work threshold; a query that finishes earlier launches no helper. This policy uses the current query's progress and no learned state from previous queries. Recognition continues without restarting or waiting.

`Selective` remains an explicit experimental policy: it uses recent proof consumption to back off within an input-size class. A workload change can make that history misleading. `Always` retains admission on every eligible query as a control. Admission and query schedules can affect cost or which optional proof arrives before a resource limit, but cannot turn an unproved result into a completed verdict.

`wait_idle()` is an explicit maintenance/measurement barrier; ordinary matching does not call it. General `Program` routing remains separate. See [ownership, cancellation, limits and examples](ATOMIC_ASSISTANCE.md).

## Limits and ablations

| Domain | Default controls / meaning |
|---|---|
| Regular compilation | 4096 NFA states, 512 DFA states, expansion depth 2048; extra fragment analysis 65536 units |
| Root integer proof | Separate 256-step allowance; exhaustion means no conclusion |
| Scalar compatibility API (`RunOptions.match`) | 200000 cumulative state units, 2000000 work steps, repeat cap 10000 |
| Native shared occurrence (`RunOptions.occurrence`) | 1000000 cumulative allocation units, 4000000 steps per attempt |
| History occurrence | Native-sized state/work caps plus 4000000 occurrence units, repeat cap 10000, active call cap 1024 |
| Residual affine search | 100000 candidates and 64 MiB of conservative validation spans |
| Optional assistance | Separate helper allowance; native state/work caps remain intact |

Counters charge logical work/allocation, not peak heap bytes. The unified scalar adapter counts cumulative VM allocations, replacing the archived scalar evaluator's peak-live units. Historical numerical quota thresholds and work counters are not interchangeable. Scalar compatibility queries retain an explicit unfolding cap. With `bound_scalar_repetitions=true`, a certified nullable write-only body can normalize huge bounds to `remaining_bytes + 2*live_capture_slots + 1` before unfolding. This preserves complete scalar outcomes; history and observed repetition counts are excluded. Disable that option for a same-binary control. Pure native queries use state/work caps rather than the VM's history-depth/repeat controls.

If a native query exhausts after using optional acceleration, its attempt is
released and the used tier is disabled monotonically as follows:

| Tier | Disabled for the retry | Counter |
|---|---|---|
| Source view | `normalized_exact_programs`; selected candidate is cleared | Selected executor-family counter; see normalized-program diagnostic below |
| Bounded words | `bounded_word_prediction` | `native_bounded_word_fallbacks` |
| Deterministic program | `deterministic_programs` | `native_exact_program_fallbacks` |
| Regional | `regional_execution`, `regional_constraints` | `native_regional_fallbacks` |
| Exact spine | `exact_end_spines` | `native_spine_fallbacks` |
| Exact trailing rim | `predictive_exact_rims` | `native_predictive_rim_fallbacks` |
| Predictive | `predictive_recursion` | `native_predictive_fallbacks` |
| Consumer | `endpoint_consumer_guards` | `native_consumer_fallbacks` |
| Frontier | `inline_nullable_frontiers`, `aggregate_interval_runs`, `aggregate_prefix_blocks` | `native_frontier_fallbacks` |
| Ordinary | `local_regular_regions`, `aggregate_returns`, `aggregate_suffix_returns` | `native_ordinary_fallbacks` |

Unused tiers are skipped. Every retry receives the full original per-attempt
state/work allowance. The conservative maximum is ten retries and eleven attempts;
endpoint enumeration cannot activate the five root-certificate tiers or the
regional tier. This protects the
specified preceding configurations under the selected remaining controls. It
does **not** promise the smallest quota achievable by every arbitrary combination
of ablation switches; retained mixed-configuration counterexamples are described
in the control-isolation note.

Primary counters describe the final attempt. Add
`native_acceleration_discarded_states` and `native_acceleration_discarded_steps`
for total charges. `native_acceleration_fallbacks` sums the executor-family
counters. `native_normalized_program_fallbacks` is an additional, overlapping
diagnostic and must not be added to that sum. A source-view predictor failure
can count toward `native_predictive_fallbacks` before an original-source
predictor retry, so a family counter is not universally bounded by one.
Endpoint export uses the applicable same
tiers. To request only the ordinary native attempt, disable the
representation switches listed in the table. `predictive_continuation_runs` and
`endpoint_consumer_admission` then have no active parent mechanism.

Index expiry, closed-result retention and the implicit request agenda remain
configured across retries; they do not add state/work charges to an otherwise
identical schedule. Failed optional work has the conservative bound of ten full allowances
before a last attempt, so fallback protects capability rather than latency.
A resource limit is unresolved, never a negative language answer. The separate
integer certificate only declines when its local allowance is exhausted and
does not add a native retry.

`native_agenda_tasks` counts physical task allocations; `up_steps` also counts
logical members resumed from aggregated tasks. A fall in one does not imply a
fall in the other. Local DFA transitions and persistent cursor lifetimes have
separate `regular_region_*` counters. Compilation cost and process memory are
separate from these query budgets.

Within native requests, `prune=false` disables optional arithmetic rejection and pure-query boundary/pair checks. Productivity, minimum feasibility and exact end-demand semantics remain, so this is not an ablation of all arithmetic. Disabling pruning does not make Boolean or endpoint queries enumerate capture outcomes. Scalar projection removes unobserved slots, but explicit path syntax currently enables full occurrence history for the whole VM query; it is not a per-region liveness analysis. A shared request's identity includes its exact end demand or its unrestricted-end mode. Never drop that distinction when caching.

`RunOptions.optimize=false` disables the root proof stage and optimized Auto
routing and clears the translated native `prune` flag. It is not a blanket
ablation of every native representation: disable `exact_end_spines`,
`predictive_exact_rims`, `predictive_recursion`, `bounded_word_prediction`,
`deterministic_programs`, `regional_execution`, `regional_constraints`, and
`endpoint_consumer_guards` explicitly when those mechanisms must be absent.
Conversely, `occurrence.prune=false` alone does not disable the separate
`Program` root certificates.

A start offset beyond subject length is an error. A limited endpoint query does not return a partial vector. Witness extraction can itself exhaust after membership would succeed. Caller code should handle `ResourceLimit` separately from a completed false result.
