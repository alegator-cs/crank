# Crank and Marpa::R2 Scanless: independent frontend benchmark

This package contains compact **historical measurements from 2026-10-12** and
a portable harness for **new runs on public synthetic fixtures**. These are
different inputs. Generating or running the public fixtures does not reproduce
the historical numbers below, and the numbers do not measure later Crank patches.

The comparison is C++ Crank's public grammar-source API versus the Perl/C
Marpa::R2 Scanless service, using independently authored matched grammars. It
requires Boolean whole-input recognition: no captures, user semantic actions,
schema checking, input-regex execution, or parse-forest enumeration. Marpa parses
its own DSL through `Scanless::G`, creates a fresh `Scanless::R` per input, and
checks the nonnullable root's completion at physical EOF. It never calls Crank's
expression parser. One-byte lexemes and no discard preserve the byte-language
contracts. This measures complete frontend service, rather than isolated core
recognizers or the older direct-byte libmarpa adapter.

## Historical results

The measured Crank source is `f57ac0a1552154fc27b27f5293b2f09704e8ae1e`, GCC 11.4,
`-O2`, Ubuntu 22.04 x86-64. Marpa::R2 is **14.000000**, bundled Libmarpa **11.0.2**,
threaded Perl 5.34. The recorded default enables Leo; individual Leo-item use was
not instrumented. Expression-preparation changes and the separate experimental
correctness patch were not assumed integrated in these measurements.

Each row is a separate phase. They reuse fixtures, alter accounting/warning
controls, or target a discovered loss. **Do not pool them into a global score.**
`M/C` is the weighted geometric mean of Marpa/Crank warm wall time; `C/M` is the
weighted arithmetic mean of normalized Crank/Marpa warm wall time.

| Phase (saved CSV) | Complete pairs Auto / Progress4 | Warm wins Auto / Progress4 | Geometric M/C Auto / Progress4 | Arithmetic C/M Auto / Progress4 |
|---|---:|---:|---:|---:|
| [Initial 32](results/initial32.csv) | 32 / 32 | 31/32 / 31/32 | 98.7x / 26.7x | 0.138 / 0.169 |
| [Configured wide 64](results/configured64.csv) | 62 / 58 | 61/62 / 56/58 | Full index withheld | Full index withheld |
| [Wide 64 hardware control](results/hardware64.csv) | 64 / 64 | 64/64 / 63/64 | 118x / 25.6x | 0.0997 / 0.160 |
| [Common-count 32](results/batch32.csv) | 32 / 32 | 31/32 / 31/32 | 127x / 29.9x | 0.138 / 0.171 |
| [Real local files 24](results/local24.csv) | 24 / 24 | 24/24 / 24/24 | 3.22e3x / 41.6x | 8.83e-4 / 0.0794 |
| [Targeted regex 20](results/regex20.csv) | 17 / 17 | 17/17 / 17/17 | Full index withheld | Full index withheld |
| [Same regex, warning control](results/regex_warning_control20.csv) | 14 / 14 | 14/14 / 13/14 | Full index withheld | Full index withheld |

The initial and common-count suites balance eight families, two sizes, and
accepted/malformed outcomes equally. The wide suite has roughly 64 KiB and
512 KiB structured inputs plus 256/1,024-byte regex syntax. Within family/size,
acceptance gets half the weight; early/middle/late rejection split the other half.
Configured-wide paired coverage is 95.8% Auto and 87.5% Progress4. Its explicitly
renormalized covered-subset geometric M/C is 127x/28.6x; these are not full-corpus
indices. The local-file suite balances INI/CSV families and files within family.

Warm singleton estimates are the median of three process medians, each from
seven fresh distinct inputs. The common-count phase uses **COUNT=128**, five
samples per process and 1,920 warm inputs per mode/entry. Its samples and tails
are **batch-average costs per input, not singleton latency or individual latency
tails**. Helpers drain after each query, including inside the batch loop.

Cold is public grammar preparation plus the first **completed** input. Marpa
module import is subtracted inside each process before taking the median.
Import-inclusive cold, public preparation, first-query cost, and child lifetime
remain separate columns. Cold excludes process startup, file loading, later
diagnostics and persistent-program teardown. Neither warm mode parses grammar
source. CPU columns use process CPU, including helper work; wall columns use
monotonic time. All completed pairs favored both Crank modes on cold wall time.

Auto/Marpa used CPU mask `[0]`; Progress4 permitted `[0,2,4,6]` and four workers
including caller. Admission starts after 512 native steps with a 1,024-byte floor.
Pool counters establish work/admission/drain, without proving four-core overlap
or foreground consumption of published proofs. Assisted service includes drain;
warm foreground Boolean-return latency before drain was not measured separately.

### Losses and limitations

| Credible or nominal warm loss | Auto / Progress4 / Marpa wall cost | Crank/Marpa | Assistance |
|---|---:|---:|---|
| Initial 79-byte unmatched-close regex | 576.7 / 550.3 / 365.3 us | 1.579 / 1.506 | Correct SmallInput decline |
| Common-count confirmation of that family | 562.5 / 558.0 / 386.5 us per input | 1.456 / 1.444 | No helpers |
| Hardware-control 1,024-byte early regex rejection | 35.0 / 175.3 / 89.4 us | 0.391 / 1.961 | Helpers admitted |
| Targeted warning-control early duplicate postfix | 79.3 / 157.3 / 126.1 us | 0.629 / 1.247 | Helpers admitted; nominal loss |

The first three rows have separated process-median ranges; the assisted early
rejection also lost with configured allowances (1.74x). The last row has
overlapping ranges and won with warnings enabled, so that loss is unstable.
Near-parity wins can also change sign. Tiny Auto costs, including sound
constant-time suffix rejection, make reciprocal ratios timing-floor sensitive.
These selected byte-language fixtures do not establish universal practical
dominance. No causal bottleneck decomposition was measured for the 79-byte loss.

Eight configured-wide first queries hit Crank's cumulative **2,000,000-state**
accounting allowance; the harness also configured 8,000,000 foreground steps.
These were `ResourceLimit`, not memory exhaustion or 180-second timeouts. Marpa
had no equivalent logical quota. The hardware-control phase uniformly reran all
64 entries with foreground `max_states/max_steps=SIZE_MAX`, retaining the same
inputs, weights, 2 GiB process address-space limit, 180-second cumulative
mode/entry allowance, Progress policy and 4,000,000 helper-step budget. All eight
resolved; 576 processes completed. The eight original failure rows remain in
the configured CSV. **The control is benchmark configuration, and makes no
production-limit change.** Zero would be a zero allowance in this engine.

| Phase | Marpa processes with warnings / total | Warning lines | stderr text bytes | SIGABRT |
|---|---:|---:|---:|---:|
| Initial 32 | 0 / 96 | 0 | 0 | 0 |
| Configured wide 64 | 9 / 192 | 5,184 | 1,161,216 | 0 |
| Wide hardware control | 9 / 192 | 5,184 | 1,161,216 | 0 |
| Common-count 32 | 0 / 96 | 0 | 0 | 0 |
| Local files 24 | 0 / 72 | 0 | 0 | 0 |
| Targeted regex 20 | 42 / 54 | 45,183 | 10,123,830 | 3 |
| Regex warning control | 0 / 44 | 0 | 0 | 2 |

Warning I/O affects three regex entries in each wide phase. It occurs inside
recognition service and is a timing confound; no per-warning timing decomposition
was measured. The targeted 4,096-byte family produced three SIGABRT outcomes
before Boolean receipts, about 14.4–14.8 seconds each. Their stage/cause is unknown:
they are crashes, not `NoMatch`, proven memory failures, or timeouts. Paired mass
is 81.25%; covered-subset geometric M/C is 3.48x/6.14x, with no full index.

The compact [warning and abort audit](results/WARNING_AUDIT.json) preserves exact
phase counts and signal receipts. Warning text is ASCII, so recorded character
counts equal UTF-8 text bytes.

The separate same-input warning control changes only the Marpa recognizer's
`too_many_earley_items` threshold to **1,000,000,000**, in both lean and diagnostic
constructors. It admits 16 of the 20 entries: Crank completes 16, Marpa 14. Two
4 KiB accepted/EOF-close attempts again SIGABRT with empty stdout/stderr.
Warning output is therefore not necessary for those aborts; their cause remains
unknown. Four late-position entries are pending for every mode because a complete
group did not fit the global reservation. This is budget censoring, not failure.
Paired mass is 71.875%; covered-subset geometric M/C is 3.09x/4.61x. The diagnostic
phases stay outside the general aggregates.

The fixed historical campaign shared a finite 7,200-second window including
setup and all phases, starting 18:44:47 UTC. Complete groups reserved all three
mode allowances. An earlier five-minute campaign covered only half its planned
weight; its shrinking-deadline kills do not show equal-budget throughput and
are excluded here. Resource, timeout, crash, cancellation and pending outcomes
must remain distinct.

## Public inputs and privacy boundary

Historical synthetic families include regex syntax, nested lists, event streams,
fixed-vocabulary XML and arithmetic. Initial larger CSV/INI use local measurement
tables and a CPAN configuration; larger initial JSON projects local install
metadata. Wide JSON includes an intact installation receipt and historical-case
JSON. The local-file phase uses five CPAN configuration files and one historical
CSV. This is a convenience local-artifact sample, not sampled production traffic.
One non-ASCII YAML package configuration was excluded before timing under the
ASCII contract; that exclusion is not `NoMatch`.

The complete private evidence retains original corpora and raw journals. This
public package withholds those corpora, CPAN sources, distributions and binaries,
full installation/environment receipts, raw journals, and host-specific paths.
In the common-count CSV, 24 provenance cells replace only `source_path` with
`<withheld-local-source>`; numerical measurements, status/failure rows, source
hashes, mutations and weights are preserved. [PROVENANCE.json](results/PROVENANCE.json)
records original/public CSV hashes and the unchanged measured runner hashes.
Offline recomputation verifies every phase's pair counts, wins, coverage and
weighted CPU/wall indices. Public summaries are compact extracts from the saved
reports, not newly measured results.

The authored harness and independent grammar/oracle code are covered by the
repository license. No third-party CPAN source/configuration is vendored here.
The synthetic CSV/INI/JSON seeds read no local files, URLs, receipts or engine
outputs. The initial JSON grammar is restricted raw ASCII; the wide JSON grammar
admits all printable ASCII raw string bytes except quote/backslash, requiring
standard JSON escapes. Neither imposes a Unicode scalar, duplicate-key or schema
policy. New public wide JSON content and mutations differ from historical data.

The canonical [grammar source bindings](GRAMMAR_SOURCES.json) retain all 18
Crank/Marpa grammar files at the exact hashes in the final initial-v4 and wide64
frozen manifests, including premeasurement JSON, XML and arithmetic setup
revisions. Contract modules read these verified files. Static checks require a
single-symbol START wrapper, its root completion event, no START on a G1 RHS,
and valid separate nullable-rule declarations. Marpa's runtime DSL parser remains
the authority for grammar preparation.

## Reproduce the harness on new fixtures

Use Linux, Python 3, a C++20 compiler, Perl, and an **already available** Marpa::R2
installation. This package installs nothing. If needed, point `PERL5LIB` at your
existing compatible prefix; do not reuse historical resource grants. Inspect
available CPU IDs and choose one CPU for Auto/Marpa or four distinct CPUs for
Progress4. Commands below run from the repository root:

```sh
python3 benchmarks/marpa_scanless/verify_results.py
python3 -m unittest discover -s benchmarks/marpa_scanless -p 'test_harness.py'
python3 tools/build.py libcrank
python3 benchmarks/marpa_scanless/build.py
python3 benchmarks/marpa_scanless/generate.py benchmarks/marpa_scanless/generated-smoke --suite smoke
python3 -c 'import os; print(sorted(os.sched_getaffinity(0)))'
# Replace 0,2,4,6 with four available IDs. Run only in an available CPU slot.
python3 benchmarks/marpa_scanless/run.py benchmarks/marpa_scanless/generated-smoke/manifest.json benchmarks/marpa_scanless/run-smoke --cpus 0,2,4,6 --budget-seconds 60 --entry-seconds 3
```

`--suite smoke` contains the 16 small accepted/malformed entries; `practical`
contains 32 entries; `wide` has 64 entries with early/middle/late rejection.
Input generation runs independent oracles and freezes CFI/grammar hashes before
timing. The output directory must be new. The generator caps corpus size at
512 MiB and preserves 3 GiB free. All three engines receive the same CFI bytes.
Expected Booleans are checked after recognition, not used as matcher hints.

The sequential controller defaults to 60 seconds overall and 3 seconds
**cumulatively per mode/entry across repetitions**. It reserves each selected
mode's full allowance before admitting an entry; pending groups are unmeasured.
It applies affinity, nice priority, 2 GiB address space and a 32 MiB per-log-file
output bound. A signal crash at that output bound is still a crash, not a
recognition verdict. Logs, warnings, source/input hashes, CPU/wall costs and
per-mode status remain separate. The controller's new limits and replacement
inputs differ from the historical campaign; never substitute its measurements
into the saved tables. `--families regex_syntax` is a bounded one-family smoke;
`--engines auto,progress4` performs only Crank checks and supplies no Marpa ratio.

`build.py` validates the library against its repository build receipt and checks
every recorded source/header dependency before linking. Its frontend receipt
records the library/binary hashes, dependency content hashes, compiler command
and flags, revision and dirty state. `run.py` verifies the selected binary and
runner source against that receipt, copies and embeds the exact receipt in the
campaign, binds the chosen Perl runner hash, and records observed Marpa/Libmarpa
versions through a bounded read-only probe. Version fields from actual Marpa
receipts remain separate. Later builds cannot erase the saved campaign identity.

For common-count timing, generate a new practical corpus with `--count 128
--samples 5`, build with `build.py --variant batch`, and run with `run.py --variant
batch`. The per-input helper-drain loop is preserved. For a separate accounting
control use `build.py --variant hardware-control` and `run.py --variant
hardware-control`, retaining external limits. `--quiet-marpa` selects the separate
raised-warning-threshold runner. Keep both controls labeled and independent.
No command changes production defaults or claims exact reruns of withheld inputs.
