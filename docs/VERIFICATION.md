# Verification status — 9 October 2026

The source reorganization preserves all 59 canonical C++ test names. The engine
source hashes, existing README introduction and human-supplied license are
unchanged. Checks used GCC 11.4 on Ubuntu 22.04/WSL, `-O0 -g0`, one CPU and a
2 GiB address-space cap. These are correctness checks, not performance results.

All 23 affected regression targets compiled and linked. Twenty of the 22
executed targets completed successfully. Two were stopped by an external
45-second watchdog; the nullable-frontier target was compiled only. The full
canonical suite is **not verified complete** by this run.

| Test | What it checks | Latest execution status | Earlier saved outcome |
|---|---|---|---|
| `test_scalar_bounds` | Scalar capture outcomes against an independent oracle and unclamped control; 4,433 differential cases, huge repetition bounds, last-capture behavior, and count/path/trace admission gates | Incomplete: watchdog sent SIGKILL at 45.011 s, exit −9 | Passed 13,333 checks with the same compiler/flags and unchanged source; no individual elapsed time was recorded |
| `test_relation_oracle` | Independent least-fixed-point position relations, full Boolean answers, every-origin endpoints, witnesses, multiple entries and exact intervals; default 60 grammars × 39 profiles | Incomplete: watchdog sent SIGKILL at 45.008 s, exit −9 | Passed 2,124,505 checks / 2,124,502 native queries; no individual elapsed time was recorded |
| `test_nullable_frontiers` | Nullable-frontier on/off equivalence across 26 patterns × 261 subjects × eight modes, endpoint origins, exact-budget fallbacks and observer exclusions | Compile/link only; execution deliberately skipped | The earlier canonical command stopped during this test at its 1,200 s **whole-suite** cap; this is not a measured 1,200 s runtime for the individual test |

The two current capped runs produced no assertion failure, exception report or
engine `ResourceLimit` result. They print success summaries only after finishing,
so the interrupted runs do not expose their progress. Their large exhaustive
matrices and the unoptimized build make the short cap a plausible explanation;
the timeout alone cannot establish whether execution was merely slow or stuck.
The watchdog is part of this bounded audit, not `tools/test.py`. `--quick`
reduces the later Python corpus, not these C++ tests.

A low-priority diagnostic recompiled the original, pre-move versions of all
three tests with the same flags. Each original object was **byte-for-byte
identical** to its relocated object; engine inputs were unchanged. This provides
direct evidence that relocation did not change their executable test code. A
reduced relation-oracle run (`--random 2 --length 2`) also reached its separate
30-second cap without failure output; that subset is not marked passed. No long
full-test rerun was started during this diagnostic.

The reorganized spine target was rebuilt and passed after its optional Marpa
include moved to `adapters/`. The public CLI and relocated regular reference
quick checks passed earlier during the `reference/` rename.

The benchmark harness passed seven tests. Correctness-only runs completed all
52 selected engine/case checks: four Crank smoke cases, 28 practical cases,
eight arithmetic/record rejection controls, and 12 native/Progress-assistance
checks. No warm timing campaign ran. The historical Marpa figure redraw was
pixel-identical to its retained original; its measurements remain historical.

Pinned external comparison execution remains unavailable on this machine:
PCRE2 is 10.39 rather than 10.42, Hyperscan 5.4.2 is absent, and no pinned
Libmarpa source/build prefix was supplied. PCRE2 wrapper syntax was checked with
the installed headers; that does not verify pinned runtime behavior. The
[dependency instructions](../benchmarks/DEPENDENCIES.md) describe manual builds;
no libraries were installed. Missing dependencies, resource limits and timeouts
are nondecisions, never negative matches.

File hashes, build paths, quoted includes and new relative documentation links
were audited. `research/`, `experimental/` and `marpa_comparison/` are absent;
the canonical regressions and their helpers live in `tests/`, while external
comparison wrappers live in `adapters/`.

## Capped commands and the next useful diagnostic

The interrupted commands were `./build/hybrid/test_scalar_bounds` and
`./build/hybrid/test_relation_oracle`, with no test-specific arguments. The
audit watchdog applied 45-second wall caps and sent SIGKILL to its own process
group. The reduced command was
`./build/hybrid/test_relation_oracle --random 2 --length 2`, with a separate
30-second cap. The earlier 1,200-second cap applied to
`python3 tools/test.py --quick` as a whole, including compilation and preceding
tests. These statuses are incomplete execution, not a pass or an assertion
failure.

The smallest useful future instrumentation is a flushed stderr START/END
record around each preparation/oracle/native call, with a completed-case
counter and a steady-clock duration. Scalar bounds needs the fixture ID,
subject index/length, repetition range and clamped/unclamped phase. Relation
oracle needs grammar ID/seed, profile, subject index, query kind and origin/end;
nullable frontiers needs pattern ID, option mode, subject index/start and the
frontier on/off phase. The last START without an END identifies the consuming
call even if it never returns; periodic completed-case counts expose progress
between calls. This instrumentation was not added to the canonical tests in
this reorganization, and its output would be diagnostic evidence rather than
a performance measurement.
