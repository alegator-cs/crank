These are saved September 16, 2026 comparison results recovered from the complete `Crank_Certified_Demands (6).zip` archive. They are historical evidence, not timings measured on the current release. No performance job ran during recovery.

The full reports remain in the source archive as `crank/certified_comparison/CROSSOVERS.json` and `CROSSOVERS.md`; they are not copied into this compact package. `selected-results.json` retains all 36 original rows for four selected rejection fixtures. `fixtures.json` describes exact patterns, input bytes, generators, hashes, and budgets; `fixtures/` holds the actual ASCII inputs. `correctness.json` records byte regeneration and independent language checks. All saved modes resolved each selected fixture to `no_match` in all three processes.

| Fixture | Crank Auto warm, us | PCRE2 JIT warm, us | Hyperscan warm, us | Crank Auto cold, us | PCRE2 JIT cold, us | Hyperscan cold, us |
|---|---:|---:|---:|---:|---:|---:|
| lattice_16379 | 0.100443 | 649.200086957 | 1467.31822222 | 398.320 | 765.372 | 36490.854 |
| bounded_lattice_8191 | 0.104835 | 158.572644444 | 14.7908092933 | 384.800 | 270.147 | 15389223.115 |
| bounded_lattice_16379 | 0.052080 | 322.342386364 | 0.00967653666667 | 371.179 | 424.158 | 15375849.965 |
| record_flat_131072_final_digit | 0.055185 | 61.8224954545 | 0.0309220287146 | 84.417 | 195.675 | 1140.556 |

The first two rows are warm-query wins over both PCRE2 JIT and Hyperscan on arithmetic length holes. Crank Auto reports `integer-length-filter`. The bounded 8191 case loses to PCRE2 JIT when preparation is included. Hyperscan wins warm queries in the final two rows. Those counterexamples must remain beside the positive claim. PCRE2 interpreter, Libmarpa, Rust, baseline and native data remain in the selected saved rows.

The language contract is full-span anchored Boolean membership on byte inputs. These four ASCII regular fixtures use the same language across the compared engines. Capture histories, endpoint enumeration, search semantics, Unicode behavior and unrelated inputs are outside this claim. Unsupported, timeout and resource-limit outcomes are not negative matches.

Warm values are the median of three fresh-process medians; each process uses five calibrated batch means targeting 15 ms. Preparation and first-query timing are recorded separately. Cold totals are the original independently aggregated totals; do not add separately aggregated medians to reconstruct them. These submicrosecond values are amortized batch measurements, not per-call stopwatch measurements.

The saved production engine binding is `6851a56ddb8b0bd9bae1d47a6b35a2a24c470222`. The campaign checkout (`ad87996cfa810b101195c34409292a06c7c5286c`) and build-receipt checkout (`8038f67a482e2abf234819b0ded068c038c190b8`) are separately retained in `provenance.json`; they should not be conflated. That file records dependency hashes, binary hash, dates, compiler and machine limits. Original dependencies were PCRE2 10.42, Hyperscan 5.4.2 and Libmarpa 11.0.13 with Leo enabled, source commit `243540393a690efc2f6aca20f9d57c4099dcf646`. Original CPU was AMD EPYC 9V74, compiler Ubuntu GCC 13.3, with CPU affinity 0-3, a 3 GiB per-process address-space limit and a 20 s wall timeout.

The current portable runner lives in the repository-level `benchmarks/` folder. Original driver/generator paths and their hashes remain in `provenance.json` as archive references; historical driver sources and third-party binaries or vendored source are not bundled here. The current Marpa adapter maps the Crank AST directly to BNF, feeds one byte token per earleme, and explicitly enables Leo. Its generated private header/API must match the pinned Libmarpa source.

The original report records zero helper queries, publications and requests across its corpus. The selected speed differences support foreground admission and arithmetic filtering; they do not demonstrate a speculative-threading speedup.