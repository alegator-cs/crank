# Native accounting limits and benchmark controls

Crank's native state and work limits are cumulative logical charges per
attempt, not measurements of simultaneously live objects or heap bytes.
The limits protect bounded execution; exhausting them leaves recognition
unresolved. The October 2026 Scanless comparison found configuration limits
that explain eight incomplete Crank queries, but provides no evidence-backed
production safety fix.

## Production contract

[`OccurrenceMatchOptions`](../engine/occurrence_matcher.hpp#L9) documents the
allocation/work units and per-attempt allowances. The shared AST executor's
[`allocate` and `step`](../engine/ast_machine_hybrid.cpp#L285) reject a charge
that exceeds the remaining allowance and throw `ResourceLimit`. Zero is a
zero allowance, not an unlimited sentinel. Counters are not interchangeable
with historical peak-live state counts; see
[API limits](API.md#limits-and-ablations).

When an attempt has used optional acceleration, the
[`with_native_fallback` retry loop](../engine/ast_machine_hybrid.cpp#L2178)
disables its used tier monotonically and releases the abandoned graph. Each
retry receives the full configured allowance. Final-attempt counters and
discarded-attempt charges remain separate. There are at most ten retries;
fallback protects capability rather than a single-attempt latency bound.
An exhausted ordinary attempt still throws `ResourceLimit`.

Completed `true`/`false` results mean Match/NoMatch. A resource limit is not
NoMatch. Assisted execution can return false after native exhaustion only
when a completed whole-query rejection proof is
[`already published`](../engine/assisted_matcher.cpp#L358); it does not wait
for an unfinished helper to rescue the query. Unknown or partial helper work
cannot supply a negative verdict.

Assistance keeps an independent helper allowance and
[`admission gates`](../engine/assisted_matcher.cpp#L291). Progress admission
begins only after current native work reaches its threshold; it neither
restarts recognition nor deducts speculation from native quotas. Busy,
unavailable, small-input and zero-helper-budget paths retain native execution.
Cooperative cancellation remains distinct: cancellable execution throws
[`AstExecutionCancelled`](../engine/ast_machine_hybrid.cpp#L2211), including
the [native SLR cancellation outcome](../engine/ast_machine_hybrid.cpp#L1978).
The resource retry loop catches only `ResourceLimit`.

## Observed benchmark limits

The configured wide phase used 2,000,000 state units and 8,000,000 work steps.
Eight first-query receipts reported `ResourceLimit`, exit 5, with
`shared AST state limit exceeded` after approximately 0.44-0.52 seconds:

| Input outcomes | Mode | Input bytes | Failures |
|---|---|---:|---:|
| CSV accepted and late rejected | Progress4 | 527,262 | 2 |
| INI accepted and late rejected | Progress4 | 526,353 | 2 |
| XML subset accepted and late rejected | Auto and Progress4 | 524,440 | 4 |

Those receipts establish cumulative state-allowance exhaustion. They do not
establish memory exhaustion, peak-live state counts or a deadline failure.
Marpa had no equivalent logical quota, so these outcomes are not numerical
latency observations or proof of a time/memory-budget Marpa win.

The separate native accounting control reran all 64 frozen entries with
identical inputs, grammars and weights. Its wrapper set only the four
foreground match/occurrence state/work allowances to
`std::numeric_limits<std::size_t>::max()`. All modes completed all entries:
576 successful processes across three repetitions. The same 2 GiB process
address-space limit, 180-second cumulative mode/entry allowance, Progress
policy and 4,000,000 total helper-step allowance remained in effect.
No engine/library defaults changed.

The control's separate CSV accepted Progress4 diagnostic recorded
8,003,491 work steps, just above the configured 8,000,000 allowance. That
successful execution also exceeds the original work quota; a state-only
control was not measured, and retries can select a different execution.
This is an untimed diagnostic counter, not a decomposition of measured
latency. The original failures and the uniformly applied control remain
separate configurations in the [benchmark package](../benchmarks/marpa_scanless/README.md).

## Integration decision

Keep production defaults and outcome semantics unchanged. The benchmark
control answers a comparison question under common finite time/memory
constraints; it does not prove that cumulative limits should become live
memory accounting, scale automatically with input size, or be disabled in
ordinary use. Callers can explicitly select suitable existing allowances.
A future production optimization needs native correctness and quota evidence,
including mixed acceleration/fallback paths and cancellation, before adoption.
