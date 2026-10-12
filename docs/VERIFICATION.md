# Verification status - 9 October 2026

All 23 targets affected by the source reorganization have compiled and passed
across the serial verification runs. The three tests investigated for apparent
timeouts now finish successfully. Saved outcomes cover 58 of the 59 canonical
C++ targets; `test_scan_certificates` remains unverified in these saved runs.
The full canonical suite is not marked complete. The complete
`tools/test.py --quick` command has not finished in a single invocation; its
earlier run hit a 1,200-second overall cap, including compilation and preceding
tests.

The three tests investigated for apparent timeouts have now completed with
their original cases and internal budgets:

| Test | Latest complete execution | Scope |
|---|---|---|
| `test_scalar_bounds` | Passed in 65.973 s; 13,333 checks | 4,433 scalar-capture differentials plus 17 huge-bound, last-capture and observer-gate cases |
| `test_relation_oracle` | Passed in 264.212 s; 2,124,505 checks / 2,124,502 native queries | 60 grammars, 39 profiles, 1,849 independent reference languages |
| `test_nullable_frontiers` | Passed in 32.094 s; 896,091 checks | 26 patterns, 261 subjects, eight Boolean/endpoint modes plus witness and uncompressed-return controls; 382,582 activations |

These timings used optional diagnostic tracing, GCC 11.4 on Ubuntu 22.04/WSL,
`-O0 -g0`, one CPU, nice +10 and a 2 GiB address-space cap. They are correctness
run durations, not benchmark results. The diagnostic supervisor ran one child
process at a time and imposed separate 120/600/60-second wall caps on these
completed runs. Test work budgets and cases were not changed.

## What caused the earlier interruptions

The 45-second scalar and relation watchdogs interrupted advancing batches.
Scalar completed 2,490 cases at 45 seconds and 3,723 at 59 seconds in its first
extended trace, then passed in the later complete run. Its slower middle
fixtures repeatedly evaluate nested nullable captures and repetitions with
scalar clamping on and off; recorded whole cases reached about 0.325 seconds.

Relation's slow stretch was the 214-node `concat_above_control_word` fixture,
which repeats 70 nullable terms and checks each subject across 39 option
profiles. The `aaaab` subject took 10.592 seconds across many returning calls;
subsequent subjects and profiles kept advancing. In that extended trace,
completed scopes advanced from 447,160 at 45 seconds to 449,233 at 59 seconds.
The traces distinguished native calls from program setup and the independent
least-fixed-point matrix oracle. No native API call remained active for two
seconds in those capped trace samples. The completed runs establish that the
observed test executions finish.

Nullable passed in about 32 seconds. Its older interruption happened at a
1,200-second **whole-suite** cap, which did not measure the individual test's
runtime. Scalar's seven recorded exceptions in its completed trace were the
deliberately caught `ResourceLimit` controls required by its gate assertions;
they were not assertion failures.

Before tracing was added, recompiled original and relocated objects for these
three tests were byte-for-byte identical. The optional tracing now changes
their test source but preserves cases, options, assertions and reference
algorithms. Production engine source hashes, the existing README introduction
and the human-supplied license remain unchanged.

## Repeating the diagnostics

Build and run normally with the repository's tools. Optional tracing is off by
default and changes neither work limits nor matching outcomes:

```sh
CXX=g++ EXTRA_CXXFLAGS='-O0 -g0' python3 tools/build.py test_scalar_bounds test_relation_oracle test_nullable_frontiers
CRANK_TEST_PROGRESS=1 ./build/hybrid/test_scalar_bounds
CRANK_TEST_PROGRESS=1 ./build/hybrid/test_relation_oracle
CRANK_TEST_PROGRESS=1 ./build/hybrid/test_nullable_frontiers
```

Tracing writes `CRANK_PROGRESS` JSON records to stderr: selected risky-call
START/END boundaries, once-per-second STATE records, and a final summary.
Records identify expression or reproducible random seed, input, phase, option
profile, origin/interval and budgets. Current and last-completed IDs, elapsed
times and completed-case counts distinguish a long individual call from a
large batch. Scope counts include parse/setup, reference, native and outer case
scopes; they are not interchangeable with native-query counts. Wall caps are
external diagnostic choices, not limits in the canonical test runner.

`--quick` reduces the later Python corpus, not these C++ targets. These three
tests compare Crank with independent reference evaluators or Crank option
ablations; they do not call PCRE2, Libmarpa or Hyperscan.

## Other checks and remaining scope

The reorganized spine target passed after its optional Marpa include moved to
`adapters/`. The public CLI and relocated regular reference quick checks
passed during the `reference/` rename. The October arithmetic integration
passed 202 API calls and 2,686 assertions.

The benchmark harness passed seven tests. Correctness-only runs completed all
52 selected engine/case checks: four Crank smoke cases, 28 practical cases,
eight arithmetic/record rejection controls, and 12 native/Progress-assistance
checks. No new warm timing campaign ran. The historical Marpa figure redraw
was pixel-identical to its retained original; its measurements remain
historical.

Pinned external comparison execution remains unavailable on this machine:
PCRE2 is 10.39 rather than 10.42, Hyperscan 5.4.2 is absent, and no pinned
Libmarpa source/build prefix was supplied. PCRE2 wrapper syntax was checked
with installed headers; pinned runtime behavior is unverified. The
[dependency instructions](../benchmarks/DEPENDENCIES.md) describe manual
builds; no libraries were installed. Missing dependencies, resource limits
and timeouts are nondecisions, never negative matches.

File hashes, build paths, quoted includes and relative documentation links
were audited. `research/`, `experimental/` and `marpa_comparison/` are absent;
canonical regressions and helpers live in `tests/`, and external comparison
wrappers live in `adapters/`. Diagnostic logs and private backups are outside
the release tree; generated build outputs remain ignored.
