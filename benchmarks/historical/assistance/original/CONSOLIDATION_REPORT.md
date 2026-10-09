# Crank: native consolidation and proof-search research

2026-09-16. Measured implementation: `0f21957bcbb77e7ad2ff4a2b7cf71b64bedb37e5`.
Frozen native control: `57624079e675aa577407435d345adb69e087a546`.
Retained implementation: `c3089443fd1594691bdc904a2ef212c1cbb80ad3`.
Relative to the measured implementation, it selects Progress by default and
corrects scheduling-sensitive policy tests; measured policies were explicit.

This pass makes Crank a smaller constraint-guided recognizer with native shared
continuations and optional negative-proof searches. It archives competing runtime
implementations, unifies scalar/history execution, and removes repeated native
messages before allocating them. It adds no Earley replacement under another name.

## What is in the delivered source

| Component | Current role |
|---|---|
| Finite AST and analysis | Resolve calls without copying syntax; compile productivity, length, residue, bounded length, byte and pair conditions. |
| Exact specializations | DFA, certified scans, count/equality/affine validators and optional PCRE2 for eligible queries. |
| Native request/return graph | Share pure `(node, begin, end demand)` requests; propagate unique endpoints and compact return messages. |
| Unified continuation VM | Keep latest scalar bindings or full occurrence histories according to observable semantics; retain forward obligations. |
| Optional atomic assistance | Private scans publish monotone node-absence and whole-input-rejection facts; foreground recognition never waits. |
| Synchronous index | Retained diagnostic utility for complete literal positions/runs and necessary conditions; not a mandatory scan. |

Internal Earley, joined CFG planning, parallel DFA composition, bidirectional span
matching and compact-native alternatives are outside the live runtime. Restore
them from `archive/pre-consolidation-2026-09-16` or `archive/compact-native`. The small
regular endpoint evaluator is test-only. Crank-redux remains frozen and unchanged.

See the [concise architecture](../research/native_consolidation/ARCHITECTURE.md),
[API](../docs/API.md), [worked example](../docs/WALKTHROUGH.md), and
[research log](../research/native_consolidation/RESEARCH_LOG.md).

## Measurement contract

The corpus has **74 independently checked cases: 37 accepts and 37 rejects**.
Its 52 application-inspired cases cover JSON rows, event streams, HTTP-like
headers, filter expressions, nested tags and fixed records. Sizes extend from
about 8 KiB through 512 KiB. Six hostile near-misses preserve byte and adjacent-pair
sets while violating larger structure. These are generated syntax subsets, not
production traffic or complete security/protocol validators.

The 22 synthetic cases expose recursive topology, ambiguity, arithmetic holes
and absent alternatives. Every expectation is checked by a separate language
predicate before either engine runs. Libmarpa **11.0.13**, pinned to
`243540393a690efc2f6aca20f9d57c4099dcf646`, uses Leo and the same existing byte-token
AST-to-BNF adapter. Results do not describe a hand-written lexer or optimized
application grammar for Marpa.

Timed workloads run serially, with randomized case/mode order, three fresh-process
repetitions, common affinity to four CPUs, a 3 GiB address-space cap, and a 15-second
wall timeout for the entire fresh-process protocol (preparation, first query,
warm batches and diagnostics), not for an individual match. A protocol timeout
therefore does not establish that its first query could not finish. No CPU rlimit is used. The environment exposes an AMD EPYC 9V74 CPU;
full hardware/container/compiler and source receipts accompany the raw data.
Application native caps are 2,000,000 state units and 8,000,000 steps. Most
synthetics use 1,000,000 / 4,000,000; the deliberate Catalan limit control uses
2,000 / 8,000 with native pruning disabled. Optional proof work has its own
4,000,000-step allowance, not a deduction from native caps; the rich index also
has a 1,000,000-literal-hit cap. Every case records its settings.

Each process reports parse/preparation, first query, five warm batches, process
CPU, peak RSS and explicit helper drain separately. A warm batch reports its
mean per-query latency; per-case table entries take medians across batches and
then across processes. Aggregate speed ratios are **medians of per-case ratios**,
not averages or total-workload speedups. Both modes must complete every recorded
repetition to contribute a ratio. Limits, aborts and timeouts remain nondecisions.
Peak RSS includes preparation; CPU includes helper work. Through-idle warm totals
account for work that outlives foreground query return.

Principal controls are frozen native, current native, synchronous indexing, real
Auto dispatch, Always/Selective/Progress assistance with two total workers, and
Libmarpa. Focused controls add four-worker assistance, same-binary native coalescing
disabled, and a synchronous run of the helper's own presence scan. That last
control distinguishes cheaper proof representation from parallel execution.
The indexing control builds its rich index but uses root rejection only before
ordinary native execution; it does not feed all indexed endpoints into native.

Assisted input-copy cost is included. Owned-input preparation is not moved out of
the measured query. Warm phases permit busy-pool fallback and never drain between
individual calls. Distribution-shift sequences reuse one compiled grammar and
one length bucket, rather than training a different program per input.

## Results

Crank's strongest result is the combination of compact native recombination and
cheap rejection proofs. The evidence does **not** support enabling more threads
for every input. The new native engine is 1.046× faster than frozen native at the
median of 56 jointly completed cases; 28 improve by more than 5%, 18 are within
5%, and 10 regress by more than 5%.

| Comparison | Jointly completed cases | Median control / candidate |
|---|---:|---:|
| Frozen native / current native | 56 | 1.046× |
| Libmarpa / current native | 58 | 2.125× |
| Libmarpa / native, application accepts | 16 | 2.798× |
| Libmarpa / native, application rejects | 21 | 2.483× |
| Libmarpa / native, synthetic cases | 21 | 0.868× |
| Native / Progress, two total workers | 58 | 0.975× |
| Native / Selective, two total workers | 58 | 0.990× |
| Synchronous presence / Progress, two total workers | 26 | 0.981× |

All 37 jointly completed application cases favor native over this Libmarpa
adapter. That statement excludes unresolved cases: native completes only **37 of
52** application cases, while Libmarpa completes **48 of 52**. On synthetics,
native wins 10 and loses 11. For example, 1,024-byte nested equality takes 715 µs
in native versus 381 µs in Marpa; the mixed-prefix case takes 43.6 ms versus
26.8 ms. Native's gains are useful, not universal dominance.

Selected warm latencies below are milliseconds. A dash means that the control
was not measured; `limit` is an unresolved native resource cap.

| Case | Frozen native | Current native | Progress, 2 workers | Progress, 4 workers | Same proof, synchronous | Libmarpa |
|---|---:|---:|---:|---:|---:|---:|
| Catalan accept, n=128 (130 bytes) | 19.121 | 3.650 | 3.746 | — | — | 12.779 |
| Catalan accept, n=256 (258 bytes) | limit | 16.588 | 17.047 | — | — | 133.423 |
| Ambiguous expression accept, 64 | 3.702 | 1.818 | 1.933 | — | — | 2.566 |
| Event stream, 128 KiB, valid | incomplete¹ | 316.016 | 309.600 | 310.014 | 310.118 | 454.230 |
| Event stream, 128 KiB, late error | 322.131 | 302.452 | 0.381 | 0.203 | 0.215 | 461.836 |
| Event stream, 128 KiB, hostile error | 342.229 | 309.937 | 323.638 | 310.315 | 309.973 | 459.572 |
| Filter expression, 128 KiB, late error | limit | limit | 1.175 | 0.164 | 0.896 | 714.848 |
| Event stream, 512 KiB, late error | limit | limit | 1.192 | 0.489 | — | 1,441.270 |

¹ One frozen-native process hit the whole-protocol timeout; its other two
repetitions completed. This is not evidence that a single query exceeded 15 s.

Disabling coalescing in the **same binary** takes Catalan128 back to 19.265 ms,
and Catalan256 back to its cap. That control supports the causal explanation
of removed native work, independently of unrelated cleanup changes.

The late-event rejection improves by hundreds of times, but the synchronous
presence scan explains most of that gain. Two-worker Progress has no >5% win
over that scan in their 26 jointly completed controls. Four workers do have
useful cases: on the 128 KiB filter error they take 164 µs versus 896 µs
synchronously. On an 8 KiB event error the order reverses: synchronous 14 µs,
Progress with four workers 52 µs. These are workload-specific crossover points,
not a single input-size threshold that works for all grammars.

The hostile event input preserves the scanned byte/pair information. Progress
with two workers then takes **4.4% longer** than native and eliminates no native
work. The helpers cannot prove the missing long-distance ordering constraint.
The large positive tail and ordinary cost are both real.

### Preparation, CPU, memory and completion coverage

For the 128 KiB event error:

| Mode | Preparation, µs | First query, µs | Warm query, µs | Process CPU/query through idle, µs | Peak RSS, KiB |
|---|---:|---:|---:|---:|---:|
| Native | 30.4 | 332,659 | 302,452 | 301,774 | 101,448 |
| Progress, 2 workers | 103.6 | 468.6 | 381.2 | 631.4 | 24,656 |
| Progress, 4 workers | 166.7 | 269.0 | 202.6 | 421.0 | 24,660 |
| Same proof, synchronous | 53.7 | 252.8 | 215.1 | 225.4 | 24,788 |
| Libmarpa | 474.1 | 455,777 | 461,836 | 458,499 | 537,412 |

Thus lower elapsed latency need not mean lower CPU. Peak RSS has an approximately
24 MiB process/preparation floor in this harness; use native state counts, not
small RSS differences, to assess small-case memory changes. Raw records include
each final drain and helper stop/cleanup observations; a stop observation is not
the complete lifetime of shared input reclamation.

On that event error, four-worker Progress achieves about 4,625 warm queries/s
through final idle, versus 3.31/s native. Its five-query batch takes 1,081 µs
with a 0.11 µs final drain (0.06–0.12 µs across repetitions). A tiny drain does
not establish zero cleanup cost: helpers often finish while foreground work
unwinds, and CPU through idle still counts their work.

| Mode | All cases completed / 74 | Application cases completed / 52 |
|---|---:|---:|
| Frozen native | 56 | 36 |
| Current native | 58 | 37 |
| Synchronous rich index | 63 | 41 |
| Auto dispatch | 62 | 41 |
| Each two-worker assistance policy | 62 | 41 |
| Libmarpa | 70 | 48 |

Only Catalan256 is a demonstrated new native budget resolution versus frozen
native; the other difference is the single protocol timeout. No query completed
by current native became unresolved under assistance in this corpus.

The main run contains 2,178 records, with no incorrect completed answers: 857
matches, 948 nonmatches, 360 limits, 12 Libmarpa aborts, and one protocol timeout.
The aborts are the four 512 KiB HTTP source forms, each repeated three times,
under the common 3 GiB address-space cap. A separate same-binary diagnostic
reproduced the cap failure and recognized the valid called form with a 6 GiB cap:
5.897 s first query, about 4.29 GiB peak RSS, 42,912,067 Earley items, Leo enabled.
The main results retain the failures; the diagnostic is not substituted into
the headline timings.

Equivalent flattened 512 KiB HTTP/fixed-record patterns select DFA in Auto and
finish in approximately **0.87–0.90 ms**, including cases whose native runs reach
the cap. The acyclic subroutine spelling currently misses that dispatch. This
is a concrete representation/dispatch opportunity, and those DFA timings must
not be attributed to native recursive recognition.

See the [complete case tables](results/tables.md), [scale and coverage tables](results/scale_tables.md)
and [address-cap diagnostic](address_cap_followup.json).

### A promising concurrency refinement that failed its timing test

The final follow-up tried publishing “all helpers finished with an empty table.”
After observing this quiescence fact, foreground execution could stop looking for
exclusions. The invariant passed independent review, ASan/UBSan and TSan; it kept
native state/work budgets unchanged and required no asynchronous cancellation.

The same-binary ablation comprised 150 fresh-process observations over ten cases,
with three repetitions and explicit enabled/disabled arms. It produced 135 correct
completed answers and 15 equal-cap JSON limits, with no wrong answers. On the
hostile 128 KiB event, it eliminated **984,008 → 3,617 checks** while retaining
exactly 1,773,398 native steps. Yet Progress latency changed **312.88 → 322.04 ms**,
and CPU changed **311.98 → 321.08 ms**. Valid events were essentially unchanged;
hostile tags also slowed. Across cases with actual retirement there was no
consistent time or CPU benefit. Apparent wins on useful-proof cases had zero
retirements and cannot support this mechanism.

**The optimization is rejected from the delivered runtime.** The failed hypothesis
is informative: removing a million cheap reads does not necessarily remove the
dominant computation. The experimental implementation survives on
`archive/empty-helper-retirement`; its [design, patch, checks and measurements](../research/native_consolidation/empty_helper_experiment/README.md)
remain available. A zero-length object artifact caused an initial sanitizer link
failure; the failed receipt is retained, the builder now rejects empty objects,
and a clean rebuild passed. No compiler/runtime cause is asserted for that artifact.

### Query history can hide the cases that matter

The separate same-program sequences contain **6,480 independently checked queries**
in 135 records, all correct. Following valid training with same-sized event errors
causes Selective to back off on all 48 measured errors. Their median latency is
6,173 µs, essentially native's 6,155 µs. Always takes 40.9 µs; Progress 62.8 µs;
the synchronous scan 15.6 µs. Progress admitted 47 queries and fell back without
waiting for one busy-pool query. Its mean is 213.5 µs, so the median alone hides
that fallback tail.

This justifies using current-query **Progress** as the assisted API's default,
while retaining Selective as an explicit experiment. Progress is not the fastest
median policy: it avoids the training blackout, but can still launch useless work
on slow valid inputs. Ordinary `Program`/Auto remains available without helper
threads. Sequence counters for synchronous root rejection have a documented
stale-native-statistics erratum; timings and verdicts are unaffected, and those
counters are excluded from analysis. See [sequence results](sequences/tables.md).

## Computational work removed

| Change | Invariant | Work that disappears | Limitation |
|---|---|---|---|
| Completion/continuation bits | Pure consumers observe caller, endpoint and control boundary, not the child derivation's identity. | Duplicate agenda entries and dispatch; some repeat/concatenation continuations. | Witness/history paths retain distinctions; metadata can cost more on sparse queries. |
| Frozen destination-word groups | No new subscribers arrive after that request's start position closes. | Traversal of return links whose destinations are already scheduled. | Nullable current-position replay remains necessary; sparse groups still require visits. |
| Ordered endpoint vectors | Pure ordered execution publishes nonterminal endpoints monotonically. | A hash lookup/insertion used solely for duplicate detection. | Unordered witness execution keeps its original representation. |
| Bounded length word | Truncated nonnegative length composition is sound below 64; recursive facts reach closure. | Demands whose lengths fall in finite holes missed by gcd/wrapping residues. | Eight extra bytes per summary; no new precision above length 63. |
| VM observation projection | Retain every value read by future matching or requested output. | Separate scalar evaluator, unobserved occurrence records and duplicate scalar configurations. | Path-sensitive histories still need full records. |
| Progress admission | Optional work starts only after current native work crosses a threshold. | Job/table allocation, input copying and wakeup for queries that finish sooner. | Slow valid queries can still pay for useless proof search. |

On the n=128 Catalan control, coalescing and word skips reduce **1,499,144 →
149,537 work units** and **401,610 → 52,234 state units**. The corresponding pure
completion task count falls **374,273 → 24,897**. These are structural counters,
separate from the measured latency table. The split-frontier case improves much
less and still reaches its configured cap; no general linear-time claim follows.

For `(a{7}|a{11})*` on 59 `a` bytes, the bounded length proof reduces **1,631
states / 2,345 steps → 0 / 1**. The recursive equivalent falls **1,847 / 2,444 →
0 / 1**. A misplaced-delimiter control sees no benefit. Analysis construction costs
**14.5% more at the median across 40 grammars**, or **10.3% on the application
subset**. This is a deliberate precision/preparation tradeoff, not free speed.
The [paired compile probe](../research/native_consolidation/proofs/compile_results.json)
contains absolute times and every sample.

## Failed ideas, costs and remaining limits

- **History-only admission is not a robust default predictor.** After unsuccessful
  valid probes at calls 1, 3, 7, 15 and 31, the Selective policy can skip an entire
  subsequent 16-query malformed tail. A consumed proof is not itself a timing win.
  Progress instead observes the current query and has no training blackout.
- **Retiring empty helper tables reduced checks but not elapsed work.** The
  150-observation same-binary experiment above did not justify the extra runtime
  protocol, so the working source keeps the simpler existing publication model.
- **Unchanged epsilon state is insufficient to skip observable iterations.**
  `(a?){3}` on `a` exposed a lost final-capture placement. That shortcut was removed
  and a regression retained.
- **A pending-message hash set repeated avoidable work.** Position epochs and word
  masks cover the useful state space without a new per-endpoint hash allocation.
- **Sorting subscribers for dense masks changes discovery order.** Consecutive
  groups preserve the original surviving order instead.
- **A giant shared span/derivation matrix was unnecessary.** The current consumer
  quotient is much smaller and does not require cross-thread graph mutation.
- **Arbitrary-width count masks, another residue modulus, general MILP solving
  and asynchronous thread cancellation were deferred.** Their costs or correctness
  obligations were not justified by this pass. No claim is made that these were
  all implemented and benchmarked.

Consolidation changes numerical scalar resource accounting from peak retained
units to cumulative VM allocation units. Large bounded nullable repetitions with
observable effects can hit limits where the removed scalar evaluator skipped
periodic layers. Pure recognition now handles huge nullable bounds by epsilon
padding; small fixed nullable cases can lose useful exact-end propagation, so
that normalization is not a universal work reduction. Resource exhaustion always
remains distinct from false.

The main unresolved costs are retained native request/return volume on large valid
or structurally hostile inputs, fanout not covered by compact destination groups,
and full history state where paths observe it. Global literal/byte/pair proofs
cannot establish long-distance ordering. More workers cannot repair that missing
information and can increase CPU, copies, scheduling and reclamation costs.

## Correctness and validation receipts

- **33 canonical C++ test targets**, full CLI contracts and **16,296** independent
  Python endpoint differential cases passed on the integrated source.
- **10,160** complete-outcome/Boolean comparisons against the frozen scalar evaluator
  passed, plus **1,306** unified-VM checks under ASan/UBSan.
- **169,273** native recombination/nullable-bound checks passed under ASan/UBSan,
  including independent recursive languages, endpoint demands, witnesses,
  coalescing ablations and exact prior budget envelopes.
- **160,133** bounded-proof checks across **98** independent analyses passed under
  ASan/UBSan, including node and suffix integer-language oracles.
- **800** admission-policy checks and **6,505** assistance checks passed under both
  ASan/UBSan and **ThreadSanitizer**. Those concurrency receipts precede the final
  thread-local nullable-bound normalization; the final native sanitizer and full
  canonical suite include that change. LeakSanitizer was disabled throughout.
- The archived empty-table experiment separately passed **615** focused checks,
  **801** policy checks and **6,505** assistance checks under ASan/UBSan and TSan.
  Those receipts validate the rejected experimental source, not an extra feature
  in the delivered runtime.
- The final retained source passed **1,056 policy** and **6,505 assistance** checks
  under both ASan/UBSan and TSan. A scheduling-sensitive admission assertion was
  corrected to allow the documented nonblocking Busy outcome; exact budget and
  callback checks remain. A subsequent zero-byte object/link failure required
  rebuilding that object before the TSan gate passed. Failed attempts and the
  successful repair are preserved separately; no sanitizer finding is hidden by
  counting a failed build as a pass.
- The optional PCRE2 build passed **207,631** external adapter checks, including
  **67,127** JIT answers, and **46** CLI capability checks.

These numbers overlap in scope and are not summed into a claim of independent
cases. [The final validation receipt](../research/native_consolidation/validation/final_receipt.json)
and [measured-revision receipt](../research/native_consolidation/validation/receipt.json)
link exact commands, logs, source/binary hashes and sanitizer scope. Failed
integration checks are described in the research log, including the huge nullable
repeat fallback regression fixed rather than removed from the tests.

## Recommended next work

1. **Recognize regular subroutine graphs without eager expansion.** The flattened
   controls establish a large dispatch gap on valid inputs, where negative proofs
   cannot help. Compile eligible acyclic calls into the existing automaton path
   with an explicit construction cap and native fallback.
2. **Reclaim closed native state and compress deterministic completion chains.**
   Profile the Marpa-winning nested, shared-prefix and large valid cases. Reduce
   retained requests and repeat fanout before changing hash tables or adding
   threads; preserve nullable replay and observable history distinctions.
3. **Try small local ordering certificates.** A per-demand endpoint interval or
   start-position exclusion may defeat byte/pair-preserving near misses. Require
   a compact consumer and bounded storage; do not first construct a global chart.
4. **Choose scanning mode from measured work.** Synchronous presence is a strong
   low-overhead control; parallel scans earn their place on some larger grammars.
   A dispatcher needs cheap current-query evidence and an explicit cost model,
   rather than relying solely on previous success or input length.

The direction suggested by this pass is a native demand-driven recognizer whose
messages represent only what callers consume, surrounded by compact necessary
conditions and optional proof searches. The largest wins came from removing work
and recognizing impossibility; concurrency multiplies those wins only when the
proof search has enough independent useful work.

## Reproduce and inspect

Start with [the reproduction guide](README.md). The final result directories retain
raw JSONL, per-case CSV tables, source/object/executable receipts, independent oracle
checks, randomized order and resource settings. Historical reports retain their
original source identities. The source archive and full Git bundle preserve the
retired implementations and the measured revision; no remote repository was
rewritten or published during this pass.
