# Selected allocation reductions: independent historical evidence

This package documents two selected changes: **active-representative equivalence
labels** and **reused DFA target scratch**. The exact selected source combination
passed the final [correctness checks](VALIDATION.json). Performance measurements
below cover independent historical variants; combined performance remains
**unmeasured**.

The historical baseline is `ee55b19be42cbc91f3537666008b3808d18db17c`, tree
`ce6ddf679f8ce61190911d9411592d771362954a`, measured on 2026-10-13.

| Independent variant | Original change | Integrated change | Changed source SHA-256 |
|---|---|---|---|
| H2C: active labels | `2bc1a338b33528c7fe4b93112c36b67b168a7766` | `b0dc28507f65125d56e9d1e5637348e07117a6bf` | `9e05db006bb744103790e2bd479877174709a08387a042c5b5953e7101bf37ed` |
| H3: DFA scratch | `1cfc70c7850fe7bd040e058161ee3c2b5ddf1042` | `bdc66cc9e6d04ba1a64e05d15b0de1b4d9d5824f` | `a1a024c509c6fbbc8b664d9bd690ec0383897a7734a2b821d65367e7785f4a75` |

The audit confirmed that each independent source snapshot differs from the
baseline only in its named file, and those file bytes match its original commit.
The request-machine source remains at baseline hash
`eb31f47785a639e02610ed90a7058facef09d6ce347fbe58491f8a3fef5293ad`.
Inline result/subscriber storage (`2be727f`) is excluded because its larger
Request footprint increases live requested payload in some cases. The historical
`final` candidate includes that excluded change. The older `h23` combination uses
the superseded fixed-label prototype. **Neither supplies measurements for the
selected H2C+H3 pair.** Their service-speed claims are omitted here.

## Measured allocation evidence

Active labels store each immutable six-word label only for active representatives,
avoiding the fixed-label prototype's source-node-sized storage cost. In isolated
equivalence preparation, nested 256 captures reduce peak incremental requested
payload from **26,848 to 20,672 bytes**; 384 inactive alias rules reduce it from
**80,536 to 62,096 bytes**. The 36-case independent equivalence probes record
10,118 fewer successful allocation calls and 260,512 fewer requested bytes in
total. These totals describe this inventory, not a workload population.

DFA scratch reuses one target vector across determinization steps, retaining the
separate source/interned-subset ownership copies. The 256 distinct-byte-chain
construction removes **66,047 calls and 4,755,384 requested bytes**, while measured
peak-live requested payload is unchanged. The 128-byte chain removes 16,769 calls
and 670,760 requested bytes, also with unchanged peak. These are allocation
reductions; they do not establish broad preparation-latency gains.

Allocation probes count successful executable-interposed C++ `new`, `new[]` and
aligned-new calls and their requested payload. Live/peak counters retain
allocation-thread attribution through deallocation on another thread. They
exclude probe headers, allocator metadata/fragmentation, stacks, libc-private
allocations and executable mappings. Logical engine states/work charges are not
allocator instrumentation. `peak_extra` is an incremental payload high-water
mark, not whole-process RSS or a memory guarantee. Foreground-created job/input
ownership counts as foreground allocation even if a helper later frees it.

## Timing controls, including losses

The [per-case CSV](independent-paired.csv) preserves **all 415 successful paired
case/phase entries for H2C and H3**, including 164 per-case median losses. Each
ratio is baseline/candidate wall time; values above one favor the candidate.
Aggregates weight unique source patterns equally within each phase. Phases
overlap and must not be pooled into one speed claim.

| Phase | H2C cases / ratio / median losses | H3 cases / ratio / median losses |
|---|---:|---:|
| Isolated equivalence | 36 / 1.232x / 0 | 36 / 1.005x / 12 |
| Full Program preparation | 36 / 1.020x / 11 | 36 / 1.014x / 14 |
| Native preparation from parsed AST | 34 / 1.004x / 19 | 34 / 0.999x / 19 |
| Direct regular preparation | Not measured in final H2C phase | 17 / 1.042x / 5 |
| Warm Auto | 28 / 0.990x / 10 | 28 / 1.013x / 12 |
| Warm native core controls | 26 / 1.009x / 8 | 26 / 1.020x / 15 |
| Warm first-witness service | 28 / 1.011x / 14 | 28 / 1.000x / 17 |
| Warm Progress4 assistance | 11 / 1.022x / 2 | 11 / 0.989x / 6 |

Unchanged-path controls vary too: the warm-service allocation deltas for these
preparation changes are zero in the saved probes. Small end-to-end ratios are
tentative; compiler/link layout, desktop load and CPU frequency were not isolated.
No broad workload speedup follows from these numbers.

The [DFA confirmation](dfa-confirmation.json) retains the important no-gain and
regression controls. For the 256-byte chain, direct regular preparation is
**1.021x**, with paired-log interval **0.947–1.149x**; full preparation is
**0.987x**, interval **0.791–1.074x**, with four of seven paired rounds slower.
The 128-byte direct preparation ratio is **1.000x**, interval **0.947–1.088x**.
All intervals span parity. They describe these seven paired batches, not general
performance confidence for arbitrary workloads.

Timing uses separate binaries without allocation interposition, GCC 11.4,
C++20, pthreads and `-O2`. The confirmation has seven randomized-order paired
trials, a 30 ms batch target and a 131,072-repetition cap. Values are batch means
per operation; they are not singleton latency tails. Allocation probes use one
operation. Preparation includes local destruction; direct equivalence/regular
and native preparation start from a parsed AST. Full Program preparation includes
source parsing. Warm service owns a new input copy each time. Assisted allocation
accounting includes a `wait_idle` barrier after each query, while the separately
recorded foreground wall time excludes that drain. This is an idle-pool workload,
not saturated throughput. Native core disables the listed accelerated execution
paths while retaining ordinary pruning/consumer constraints.

Baseline timing pairs stay within their campaign. A clean first-four-case rerun
replaces matching confirmation keys after an overlapping diagnostic. Saved
[component declines](component-declines.json) remain separate from successful
pairs; unsupported/limited preparation is not Boolean rejection or a latency win.
The historical mismatch files are empty. These tables were packaged through
static review; the later correctness run is recorded separately below.

## Failure controls and reproduction

The selected pair passed the complete GCC 11.4 `-O2` canonical suite: **61 native
targets**, full engine/CLI contracts, **16,296 reference differential cases**, and
**3,214 fresh allocation-failure/lifetime assertions**. The tested commit is
`1162c18c5b68a7da48e45d500093775a9b8c00ff`; production source hashes are unchanged
after testing. The run used one logical CPU, a 2 GiB per-process address-space
limit, a 20-minute overall bound and a 3 GiB free-space floor. Correctness
processes drained by 08:11:32 UTC on 2026-10-13. Controller Python syntax passed;
the allocation performance reproduction was deferred. Interactive desktop work
was concurrent, so these checks support correctness without new timing claims.
The [receipt](VALIDATION.json) records counts, source/binary/log hashes and scope.

The historical [fault summaries](fault-controls.json) record independent H2C
**3,214** and H3 **3,302** assertions passing. Five sweeps sample at most 96
allocation ordinals each, including first/last, distinguish propagated
`bad_alloc` from safe optional fallback, check owner reuse and exact
endpoints/witnesses, and require live payload to return after destruction.
Historical full canonical and focused sanitizer passes were on the full candidate
including inline storage; they **do not certify the selected combination**.

The canonical check for the integrated pair is:

```sh
python3 tools/test.py
```

The portable companions require only the existing repository sources/static
library and an installed C++20 compiler on Linux. They install nothing. In a
reserved CPU slot, run from the repository root:

```sh
python3 benchmarks/allocations/reproduce.py --smoke --fault-controls --output build/allocation-check
```

`reproduce.py` builds the library through the existing repository builder, verifies
its source-dependency receipt before linking, and saves executable/library/source
hashes. It builds the source companions serially, materializes two supplied
synthetic fixtures as Latin-1 bytes, runs constructor probes and the separate
uninstrumented timing controls, and optionally runs the fault sweeps. It caps the
whole route at 180 seconds by default and children at their stated bounds,
and checks for 3 GiB free before each child command. Output must be new. Probe output timing is not application
latency, and this smoke is not a statistically paired speed measurement. For a
new timing observation use the separate `timing` executable and final `timing`
argument; keep `first_*` repetitions at one. Allocation/ResourceLimit outcomes
must be inspected before interpreting any query verdict; use the fault-control
executable for failure sweeps.

Companions and corpora are frozen into the new output directory. Their hashes,
the library and production dependency hashes, and materialized input hashes are
rechecked before commands; each executable's recorded hash is checked before
launch. An owned child session permits group termination, forced cleanup and
reaping on interruption, outer timeout or failure. These safeguards bind a new
run to its actual compiled bytes; they have not been exercised by this read-only
evidence preparation.

The default budget assumes cached repository objects, as after a canonical test
run. For a fresh tree, use `--budget-seconds 600`; the library build can use the
remaining overall allowance, and the whole route still has that finite bound.

These commands run on the current chosen source tree and create **new evidence**.
To reproduce an independent variant, prepare a separate checkout of the pinned
baseline with only that selected file change and use `reproduce.py --smoke --fault-controls --repo
/path/to/that-checkout --output /path/to/new-output`. Do not substitute current
combined-tree observations into the saved independent tables. Preserve 3 GiB
free and reserve a CPU slot before any build/test/timing operation.

[PROVENANCE.json](PROVENANCE.json) binds the historical source, corpus, binary and
public-result hashes. The package contains authored synthetic fixtures, source
companions and compact data only. It omits host paths, raw logs, compiled
artifacts, historical orchestration scripts and excluded-variant results. The
repository license covers the authored companions and fixtures.
