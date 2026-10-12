# Reproducing the comparison cases

## New independent-frontends evidence

The [12 October 2026 Scanless benchmark](marpa_scanless/README.md) adds compact
saved results and portable rerun tools for C++ Crank versus Perl/C Marpa::R2.
Its independently authored grammar frontends, first/cold/warm phases, CPU/wall
costs, quota controls, losses and incomplete outcomes are distinct from the
historical byte-token comparison. Public synthetic fixtures replace
withheld local-source bytes; historical measurements keep their original
source and corpus identities.

This package separates archived measurements from a new run against the current
Crank source. It measures **whole-input Boolean membership over bytes**. It does
not compare capture extraction, capture histories, endpoint enumeration, Unicode
policies, incremental token filtering, or arbitrary production parsers.

The current practical suite reconstructs the exact **28 inputs across seven
families** used in the September 11 chart: JSON rows, event streams, HTTP headers,
filter expressions, nested tags, fixed records, and deeply nested JSON. Each
generated pattern/input is checked against its original SHA-256, then against
the original independent application-language oracle. Early, middle, and late
malformations and the original length control retain their original IDs.

## Build and run current source

Use a POSIX environment such as Linux or WSL, Python 3.10+, a C++20 compiler,
`ar`, and the normal Crank build prerequisites. Builds are sequential. `CXX` and
`EXTRA_CXXFLAGS` are honored; use the same flags for every comparison. Debug
flags are suitable for correctness smoke checks, not performance claims.

From the repository root, first check the harness without running any recognizer:

```sh
python3 -B benchmarks/test_harness.py
```

Build the current Crank runner and any already available pinned dependencies:

```sh
CXX=g++ python3 benchmarks/build.py \
  --marpa-source /path/to/libmarpa-source \
  --marpa-prefix /path/to/libmarpa-built-prefix
```

The Marpa arguments are optional. No script downloads, installs, or vendors a
dependency. Missing dependencies and optional build failures become
`unavailable` entries in `build/benchmarks/build.json`; they never become
negative match results. PCRE2 and Hyperscan are detected independently through
`pkg-config`. No GPU is used.

For concrete manual source builds into a local prefix, see
[DEPENDENCIES.md](DEPENDENCIES.md). It includes the pinned standalone Libmarpa
generation steps and PCRE2/Hyperscan `pkg-config` setup.

The primary current comparison is one command after building:

```sh
python3 benchmarks/run.py --suite practical --engines crank,marpa --plot
```

This produces fresh UTC-stamped `results.json`, generated input bytes, a summary,
and `current.svg`/`current.png` under `build/benchmarks/run-*/`. Chart generation
requires Matplotlib (the archived figure was verified with 3.10.8); it is not
needed for matching or JSON output. Fresh results describe the current **Auto**
API and pinned Marpa+Leo. The four archived Crank planner modes are preserved
only in the historical chart; this runner does not emulate those retired APIs.

A light engine correctness check avoids timing loops:

```sh
python3 benchmarks/run.py --suite smoke --engines crank --check-only
```

The regular rejection fixtures and positive controls are available separately:

```sh
python3 benchmarks/run.py --suite rejection \
  --engines crank,marpa,pcre2_interp,pcre2_jit,hyperscan --plot
```

The four original rejections are the 127/131 unbounded length hole, a bounded
feasible-range hole, an overmaximum-length control, and a final invalid digit
in a 131,064-byte record batch. Arithmetic
oracles enumerate feasible counts; positive controls admit explicit witnesses.
These cases do not establish a universal speed advantage: the archived large
bounded lattice and record-tail examples include Hyperscan warm wins.

## Late rejection with optional assistance

`--suite assistance` contains two recovered late errors and positive controls:
the 8,197-byte mixed recursive prefix with an invalid final marker, and the
131,040-byte event stream with its final `samples=[` changed to `samples[=`.
Their bytes and pattern hashes match the archived assistance campaigns.

```sh
python3 benchmarks/run.py --suite assistance \
  --engines native_ast,assisted2,assisted4,assisted_always4 \
  --cpus 0,1,2,3 --helper-steps 4000000 --plot
```

Use CPU IDs allowed by the host; `--cpus` validates that set. The default is one
allowed CPU, suitable for bounded correctness checks. A single-CPU result does
not demonstrate parallel acceleration. Workers include the foreground thread.
`assisted2`/`assisted4` use current Progress policy, admission after 512 native
steps and a 1,024-byte minimum. `assisted_always4` is an explicit Always control.
The default helper allowance is one million additional steps; the command uses
four million to match the September 16 Progress campaign allowance.

`native_ast` calls the same native request agenda directly. Nine routes that the
assisted foreground bypasses are disabled symmetrically on both controls; the
receipt lists them. This experiment is separate from Auto, which can choose
other exact fast routes. Inputs are immutable owned buffers allocated before
timers, so helper lifetime does not add a borrowed-input copy to each call.

Warm calls reuse a persistent pool and return without joining helpers. Busy
admission remains possible. The final barrier runs after timed batches; its tail
is recorded separately. `warm_wall_ns_per_query` and `warm_cpu_ns_per_query`
describe timed foreground batches (CPU includes helper work occurring during
those batches). `cpu_ns_prepare_through_timed_drain` and its wall counterpart
include preparation, first call, calibration, batches, and the final drain.
They exclude fixture input allocation and the separate diagnostic query.
Pool snapshots at the timed return and after draining expose completed helper
work and outstanding work. A warm-start snapshot and warm-interval wall/CPU
through final drain are also recorded, with the timed query count as a divisor.
There is no extra barrier between calibration and warm batches: this interval
can include helper carry-in from calibration/first query and is labeled as such.
A separate untimed diagnostic records admission,
native work, consumed culls/root rejections, publication at return, limit rescue,
and its own tail. It **does not prove helper use in timed calls**. No speed claim
should be inferred solely from an admitted helper or a diagnostic observation.

## Timing and result rules

Every selected engine/case first runs in a separate correctness process. The
supervisor compares an explicit Boolean verdict with the independent oracle.
Incorrect answers, crashes, engine errors, and malformed protocols prevent
timing. Unsupported syntax, exhausted budgets, wall timeout, and unavailable
dependencies remain separate nondecisions with `matched: null`.

Each timing repeat starts a fresh process, prepares once, measures its first
query, calibrates toward a 15-ms warm batch, then runs five reuse batches. Three
fresh-process repeats are the default. Initial engine order is seeded and
rotated between repeats. `--iterations N` explicitly overrides calibration.
Calibration is bounded at one million iterations; actual batch durations and
whether the target was capped are recorded. A fast kernel can still include
loop/verdict-check/timer overhead; inspect batch durations before drawing ratios.
The chart uses medians of per-process warm medians and medians of
preparation-plus-first-query costs. It omits any pair with a nondecision in any
requested repeat; raw successful samples remain in JSON.

Each child has a 20-second wall cap, 2-GiB address-space cap, and an explicit CPU
affinity on POSIX; native defaults are two million states/eight million steps.
All limits are configurable and recorded. POSIX timeouts terminate only the
runner's own process group. Address-space enforcement is reported as unavailable
on non-POSIX hosts. CPU latency is `null` when the process clock is unavailable.
Only one query process runs at a time. Existing explicit output files are never
overwritten. Build/source/library/binary hashes are checked before execution.

## Dependencies and provenance

`DEPENDENCIES.json` pins Libmarpa **11.0.13**, commit
`243540393a690efc2f6aca20f9d57c4099dcf646`, PCRE2 **10.42** (8-bit interpreter/JIT),
and Hyperscan **5.4.2** (block mode). The Marpa adapter is the recovered direct
Crank-AST-to-BNF byte-token adapter with Leo enabled, not a translation through a
Crank matcher. Its correctness preflight also checks the Libmarpa bocage.
It needs the standalone generated `marpa.h` and static `libmarpa.a` exposing the
private `_marpa_r_*` APIs; an arbitrary system Libmarpa package may lack them.
Supply a clean checkout of the pinned commit and a generated prefix built from
that source ([pinned upstream source](https://github.com/jeffreykegler/libmarpa/tree/243540393a690efc2f6aca20f9d57c4099dcf646)).
The runner checks the runtime version and records header/library
hashes; those hashes do not independently certify how a user-supplied library
was built. Follow the dependency's own build instructions and preserve its
copyright/license notices. No third-party source or binary is bundled here.

The first-party adapter was recovered from `crank/marpa_comparison/` in
`Crank_Certified_Demands (6).zip`; only its header include path changed. Its
active location is now [adapters/marpa_adapter.cpp](../adapters/marpa_adapter.cpp).
The PCRE2 and Hyperscan library wrappers also live in root
[adapters/](../adapters/README.md); the benchmark runner owns orchestration,
correctness gates, fixture/timing protocol, and Crank controls. External per-query
timings include the wrapper call and prepared-resource guard. They do not
promise instruction-level or latency equivalence with archived monolithic code.
`src/practical_oracles.py` is the original independent oracle. The compact
practical generator translates the original fixture-only C++ generator;
`data/practical-provenance.json` records original files and case hashes.

## Recreate the historical figure separately

```sh
python3 benchmarks/historical/marpa-practical/recreate_practical_graph.py
```

`historical/marpa-practical/` holds the September 11 original figure, selected
140-point data, and provenance. The command redraws saved data without running
recognizers. `historical/rejections/` holds selected September 16 cold/warm
records and exact inputs; its measured production checkpoint was
`6851a56ddb8b0bd9bae1d47a6b35a2a24c470222`.
`historical/assistance/` records the recovered September assistance comparisons,
their original checkpoints, policy, helper-consumption counters and limitations.
Those dates and old performance values must not be presented as a measurement
of the current source. Repeating a current command creates new evidence; it
cannot promise the same timings on another source checkpoint or machine.
