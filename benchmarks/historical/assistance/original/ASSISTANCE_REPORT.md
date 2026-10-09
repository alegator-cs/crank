# Atomic assistance: rejection wins without waiting for speculative work

The new optional assistant keeps native top-down execution authoritative. Private
helpers publish monotone impossibility bits; the foreground consumes available
proofs, continues if they are absent, and retires helpers when the answer is ready.
This removes the shared parse graph and per-query joins responsible for much of
the previous span experiment's overhead.

The results support this design for selected rejection and branch-pruning workloads. They do not
establish that speculative helpers are free: two total workers were **2.6% slower
at the median across completed cases**, while individual rejection wins reached
**5,558×** against the explicit native matcher. Four workers improved several late
scan rejections further; eight workers generally added cost. The mode stays opt-in.
An accepted recursive-alternative case also improved **12.27×**, through node
exclusions consumed during matching rather than a whole-input rejection.

## Scope and correctness

The same 178 independently checked fixtures were retained: 80 inherited synthetic,
60 application-inspired, and 38 targeted cases; 68 accepts and 110 rejects. Eight
modes ran in three fresh processes each, for **4,272 observations**. Each child
measured five warm batches, with no wait between assisted queries. Cases and modes
ran serially in deterministic shuffled order on an AMD EPYC 9V74, with a common
CPU affinity of 0–7 and an eight-core CPU quota. Source and binary integrity checks
passed before and after timing.

| Mode | Completed cases | Unresolved cases |
|---|---:|---:|
| Native | 176/178 | 2 |
| Assistant wrapper, one worker | 176/178 | 2 |
| Assistance, two workers | 176/178 | 2 |
| Assistance, four workers | 176/178 | 2 |
| Assistance, eight workers | 176/178 | 2 |
| Assistance, two workers with retained owned input | 176/178 | 2 |
| Synchronous index followed by native | 177/178 | 1 |
| Libmarpa with Leo | 178/178 | 0 |

There were **1,611 match, 2,622 no-match, and 39 resource-limit observations**,
with no wrong completed answers, crashes, or timeouts. The two native unresolved
fixtures remain `catalan_budget_unresolved_control_64` and
`split_frontier_1024_accept`. No diagnostic native-limit rescue occurred. The
assistant did not lose any completed cases relative to native under the same
native work/state caps. Helpers received an additional bounded allowance; their
work was not deducted from native progress.

Before timing, all 1,424 mode/case functional checks and the 43,218-check pinned
Marpa adapter gate passed. The implementation also passed the full regression
suite and 6,505 focused checks on both native storage variants, ASan/UBSan, and
TSan. Leak detection was disabled. See [validation receipts](../research/atomic_assistance/README.md).

## Representative query latencies

All values below are microseconds. These are medians of three process medians of
five warm batch means, excluding compilation. Each row is the same case across
modes; total worker counts include the foreground. Results are deliberately shown
for both easy-to-cull and surviving inputs.

| Case | Native | Assist 2 | Assist 4 | Assist 8 | Synchronous index | Marpa |
|---|---:|---:|---:|---:|---:|---:|
| Expression filter, late order error, about 8 KiB | 23,247.39 | 95.71 | **20.88** | 46.22 | 58.52 | 47,984.50 |
| Event stream, late order error, about 8 KiB | 6,581.61 | 30.83 | **27.50** | 51.46 | 60.08 | 28,507.52 |
| Fixed records, late order error, about 8 KiB | 4,152.76 | 81.96 | **24.93** | 48.32 | 54.21 | 11,156.61 |
| Split-frontier synthetic rejection, 1,026 bytes | 81,124.33 | **14.60** | 21.84 | 51.42 | 5.78 | 67,395.92 |
| Independent recursive alternatives, last branch accepted, 8,195 bytes | 83,119.37 | 6,775.42 | **6,323.77** | 6,590.64 | 89,000.13 | 37,546.01 |
| JSON rows, accepted, about 8 KiB | 12,915.27 | 12,578.37 | 13,262.80 | 13,193.45 | 11,615.98 | 39,388.11 |
| Event stream, accepted, about 8 KiB | 6,589.05 | 6,713.34 | 6,769.45 | 7,124.26 | 6,555.72 | 30,392.18 |
| Interior ordering error that survives global pair checks | 4,390.44 | 5,072.37 | 5,331.18 | 5,249.04 | 4,620.02 | 6,208.13 |
| Independent branches, already cheap late rejection | 6.01 | 12.68 | — | — | — | — |

Four-worker assistance improves the late expression case by **1,114×** against
native, and by **2.80×** against the synchronous index. Later input shards can
publish an interior-pair rejection before the full presence scan reaches it.
The split-frontier example improves by **5,558×** with one helper, but its
synchronous scan remains faster still. These examples establish useful concurrent
culling; they do not imply concurrent recognition of all surviving derivations.

The accepted alternative example isolates an additional benefit: the helper
rules out nodes while the root remains viable, and native execution skips their
requests. The older synchronous comparator consumes only its root rejection
result; it does not feed node exclusions into native matching. It therefore
cannot obtain this particular runtime pruning benefit despite doing an input scan.

Global presence and adjacency are deliberately incomplete proofs. Inputs with
all required literals and allowed local pairs may still violate recursive or
long-range ordering constraints. Helpers do not solve those cases; their overhead
can remain while native execution does the parsing. The 6.01-to-12.68 μs case is a
concrete admission failure: native answers so quickly that waking a helper is
counterproductive.

## Distribution and participation

The ratio is `native / assisted`: above one is faster. Completed cases receive
equal weight, so these medians are not a throughput prediction for an application.
The ±5% band is a descriptive threshold, not a statistical confidence interval.

| Completed group | Cases | Assist 2 ratio | Assist 4 ratio | Assist 8 ratio |
|---|---:|---:|---:|---:|
| All | 176 | 0.975 | 0.976 | 0.972 |
| Accepts | 67 | 0.981 | 0.979 | 0.971 |
| Rejects | 109 | 0.971 | 0.972 | 0.982 |
| Application-inspired accepts | 17 | 0.989 | 0.981 | 0.966 |
| Application-inspired rejects | 43 | 0.987 | 0.991 | 0.987 |
| Inputs meeting the 1,024-byte admission threshold | 72 | 1.034 | 1.013 | 1.000 |

Assist 2 was more than 5% faster in 46 completed cases, within 5% in 70, and more
than 5% slower in 60. A one-worker wrapper control had a 0.981 median ratio to
native; assist 2 relative to that wrapper was 0.995. Tiny wrapper costs and process
variability contribute to the aggregate as well as actual helper costs.

An idle-start diagnostic admitted helpers for 61 fixtures, including one
unresolved native case. Among the **60 completed admitted fixtures**, assist 2's
median ratio was 0.990 overall, 0.975 for 26 accepts, and 1.038 for 34 rejects.
Small inputs use native fallback. Some larger inputs reject through cheap root
facts before helper admission. Those are not parallel speedups.

Both node exclusions and whole-input certificates were consumed by native
execution. Warm queries recorded busy-pool fallbacks and cancelled helpers;
`full_match` demonstrably returned while helpers were still active. Completion
counters distinguish this from merely requesting a higher worker ceiling.

| Total workers | Warm busy fallbacks | Warm cancelled tasks | Diagnostic returns with helpers still outstanding | Largest observed stop delay | Largest culler-private cleanup |
|---|---:|---:|---:|---:|---:|
| 2 | 383 | 351 | 6 | 132.8 μs | 60.1 μs |
| 4 | 11 | 1,524 | 19 | 489.5 μs | 644.2 μs |
| 8 | 5 | 1,516 | 43 | 5,389.6 μs | 1,226.1 μs |

Counts span different calibrated query counts and are not equal-work speed ratios.
An admitted or outstanding helper is not an instrumented count of cores executing
simultaneously; this run does not measure peak concurrent execution.
The eight-worker stop-delay outlier is why bounded polling must not be described
as a wall-time guarantee. Helpers poll within 64 charged operations; scheduling,
allocation and destruction can take longer. The cleanup counter excludes final
shared job/input reclamation and pool bookkeeping; explicit return-to-idle timing
includes that tail. More workers also wake more threads and increase shared
hardware pressure even though they do not lock the parse graph.

## Comparison with Marpa and the synchronous index

Every one of the 60 application-inspired cases favored native Crank and each
assisted mode against the measured byte-level Libmarpa adapter. Assist 2's median
advantage was **3.36× on accepts** and **5.70× on rejects**; assist 4's rejection
median was **6.98×**. Synchronous indexing remained strongest across the broad
practical rejection group, with a **17.95×** median advantage over Marpa and
**2.07×** over native.

Marpa completed all cases and remains substantially better on several recursive
and ambiguous synthetics. These are the pinned Libmarpa 11.0.13 C adapter's
byte-token recognition costs, with Leo enabled. They are not measurements of a
hand-tokenized Marpa application, a production HTTP/JSON implementation, or
incremental constrained decoding. Some targeted languages are regular; this
comparison invokes the explicit native/assisted CFG machinery, not Crank's
normal DFA dispatch or RE2.

Synchronous proof scans and asynchronous assistance have different useful
regions. Short complete scans can reject before any matching allocation and
avoid thread dispatch. Parallel later-byte scans can win on larger late
impossibilities. An application selecting between them needs rejection
frequency, input size, preparation reuse, and CPU cost—not just a headline
worker count.

## CPU accounting and reproducibility

The broad run preserves CPU counters, but they are **not valid quantitative CPU
cost measurements in this environment**. Calibration found that applying the
runner's `RLIMIT_CPU` caused `CLOCK_PROCESS_CPUTIME_ID` to report stale or zero
values for short intervals. Without that limit, process and thread clocks tracked
the same controlled work. CPU affinity and the address-space cap alone did not
reproduce the failure. This does not change the monotonic wall-clock query times
or the independently checked verdicts.

A separately recorded [focused follow-up](CPU_FOLLOWUP.md) removes the CPU
limit while retaining a wall timeout and address-space cap. Its two batches have
72 fresh-process observations across six selected cases and native/2/4/8-worker modes, with no
wrong verdicts or unresolved runs. These total CPU figures include every warm
query and helper work through the final drain, amortized per query:

| Selected case | Native wall / CPU | Assist 2 wall / CPU | Assist 4 wall / CPU | Assist 8 wall / CPU |
|---|---:|---:|---:|---:|
| Late-marker rejection | 52,277.19 / 52,545.34 μs | 129.37 / 217.96 μs | 24.69 / 49.39 μs | 45.51 / 98.89 μs |
| Already-cheap rejection | 5.67 / 5.73 μs | 11.66 / 16.27 μs | 20.16 / 36.38 μs | 43.75 / 79.64 μs |
| JSON rows accepted | 11,970.94 / 12,210.34 μs | 12,456.22 / 12,428.87 μs | 12,811.38 / 13,108.18 μs | 13,054.16 / 13,137.76 μs |
| Independent alternatives accepted, separate batch | 92,271.39 / 92,951.17 μs | 6,791.10 / 7,057.01 μs | 7,196.77 / 7,702.24 μs | 6,621.94 / 6,915.29 μs |

The useful late rejection saves total CPU as well as return latency. The tiny
rejection spends more CPU and becomes slower, increasingly so with extra workers.
Accepted JSON retains its parsing cost and adds overhead. The accepted alternative
case saves **13.17× total CPU** with assist 2 by pruning absent branches. The six deliberately
selected cases are a cost check, not a substitute for the broad corpus. Wall and
CPU columns are separately summarized medians, so small cross-column differences
must not be interpreted as exact utilization.

See the [clock calibration](clock_resolution_probe.json) and
[CPU follow-up environment](cpu_followup/environment.json). Compilation,
first-use costs, borrowed-input copying, owned-input preparation, and explicit
helper drains remain recorded separately. The retained-owned-buffer mode avoids
an admitted query copy but did not demonstrate a general latency improvement in
this corpus.

See [reproduction and measurement contracts](README.md), [full case tables](results/tables.md),
[raw process observations](results/raw.jsonl), and [environment/integrity receipt](results/environment.json).
The [design document](../docs/ATOMIC_ASSISTANCE.md) gives exact certificate,
ownership, cancellation and resource-limit semantics. Earlier results and the
teaching repository remain unchanged.
