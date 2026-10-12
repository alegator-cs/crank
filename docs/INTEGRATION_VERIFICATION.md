# Integration verification — 12 October 2026

The integrated production implementation passed the full default canonical
command, with GCC 11.4 on Ubuntu 22.04/WSL, C++20 and `-O2`. Checks ran serially
on one permitted CPU at low priority with a 2 GiB process address-space cap.
The benchmark smoke uses four permitted CPUs only for Progress4 children.
More than 25 GiB disk space remained free; no dependencies were installed.

## Completed production checks

`python3 tools/test.py` completed with **all 61 declared C++ regression targets**,
including the two expression targets, all three arithmetic-chain targets, and
the budget, fallback, admission, cancellation, observer and relation-oracle
matrices. The full generated Python set was used (`quick=false`).

| Check | Result |
|---|---|
| Expression frontend | 2,308 cases: 827 accepted, 1,481 rejected |
| Expression preparation | 2,299 checks |
| Independent relation oracle | 2,124,505 checks / 2,124,502 native queries |
| Scalar repetition | 13,333 checks |
| Nullable frontiers | 896,091 checks |
| Representation budgets | 104,829 checks / 20,836 native queries, including 3,470 fallbacks |
| Main engine CLI | Auto/occurrences/unpruned: 4,225 regular checks each; DFA: 4,162; capture, reference, count, resource and invalid-syntax gates passed |
| Occurrence / capability / assisted CLI | 34 / 60 / 18 checks |
| Independent regular reference | 16,296 differential cases, 314 patterns, five large-count controls and 30 rejections |

Only the three expression implementation files change production behavior;
all production quotas, observer admission, optional acceleration/fallback and
cancellation semantics are unchanged. Tests cover their mixed interactions.
Sanitizer, optional PCRE2 and second-compiler configurations were not run.
Existing compiler warnings remain visible in the retained local log.

## Optional research checks

All five isolated probes built and passed against the same optimized integrated
`libcrank.a`. The fresh results are 19,488 scope header constructions (19,448
completed comparisons), 1,939 unchanged-path runtime checks, 6,913 slot-authority
checks, 1,794 slot execution checks and 4,499 facade checks. The latter executed
144 external-interpreter queries, with **zero native shared-view queries** and
zero timing measurements. The runtime probe was forced to `--check-only`.

These passes establish source compatibility and the tested research contracts.
They do not satisfy concurrency, asynchronous cancellation, lifetime,
trimmed-root view execution, successful quota-retry completion or balanced native
performance gates. The prototypes remain outside the production build graph.

## Harness and publication checks

The existing benchmark harness's seven tests passed. The new Scanless harness
tests passed under Linux, including nondecision classification. Offline
recomputation of all seven historical phases verified fourteen Auto/Progress4
comparisons: pair counts, wins, coverage, weighted CPU/wall indices and imported
result hashes. Results retain all losses and incomplete outcomes.

The corrected public Scanless smoke completed **48/48 processes**: sixteen
small accepted/malformed entries across all eight families, Auto, Progress4
and Marpa, with one first input and one distinct warm input per process. It
used a 60-second overall reservation, three seconds cumulatively per mode/entry,
and the existing Perl 5.34 / Marpa::R2 14.000000 / Libmarpa 11.0.2 prefix.
All eighteen public grammar files match the final frozen historical grammar
hashes, including the broad-ASCII JSON pair; replacement corpus bytes differ.
The three benchmark wrapper variants compiled successfully.

The preparation harness smoke used one paired run, four repetitions and a
0.05 ms calibration target. Every completed stage checksum and fixture /
diagnostic / decline identity agreed across baseline and integrated libraries.
These small checks validate portable tooling and do not replace or extend
historical performance tables. No smoke speedup estimate is published.

The public package excludes original local-source corpora, CPAN configurations,
third-party source/binaries, installation/environment receipts, raw process
journals, private Space/Library metadata and host-specific paths. Only 24
path-provenance cells in the common-count CSV are redacted; numeric fields,
statuses, weights and hashes are retained. Synthetic replacement seeds are
authored in this repository and do not read local source files. See the
[benchmark provenance and license boundary](../benchmarks/marpa_scanless/README.md#public-inputs-and-privacy-boundary).

The original human verification edit is retained byte-for-byte. Original README
prose and historical benchmark assets remain; new presentation is additive.
An independent static review found no production correctness, safety, privacy
or license blocker after the portable-tool corrections.

## Reproduce

From the repository root using existing dependencies:

```sh
taskset -c 0 nice -n 10 python3 tools/test.py
python3 tools/build.py libcrank
python3 research/predictive_views/run.py --build --check
python3 benchmarks/marpa_scanless/verify_results.py
python3 benchmarks/marpa_scanless/test_harness.py
python3 benchmarks/marpa_scanless/build.py
python3 benchmarks/marpa_scanless/generate.py build/scanless-smoke --suite smoke --samples 1 --repeats 1
# Select available CPU IDs and point PERL5LIB at an existing compatible prefix.
python3 benchmarks/marpa_scanless/run.py build/scanless-smoke/manifest.json build/scanless-smoke-run --cpus 0,2,4,6 --budget-seconds 60 --entry-seconds 3
```

The Scanless controller retains exact binary/library/dependency, grammar/input,
runner and observed Perl/Marpa version bindings. It reserves full per-mode
allowances, preserves 3 GiB free, and keeps resource, timeout, crash,
cancellation and pending states separate. Every new output directory must be
fresh. Portable balanced preparation commands are in the
[expression harness](../benchmarks/expression_prep/README.md).
