This package recovers two historical late-rejection fixtures and their saved assistance evidence from `Crank_Certified_Demands (6).zip`. No new performance measurement ran. Current-release runner outputs must be kept separate from these old timings.

The current-lineage example is `event_stream_131072_late_order`: a recursively defined event stream with 2,184 sixty-byte rows, 131,040 actual bytes. Its last row changes `samples=[1,2,3]` into `samples[=1,2,3]`, introducing a forbidden `[=` pair near the end. The exact grammar and input are in `fixtures/`, with generators and SHA-256 hashes in `fixtures.json`. The ordinary native matcher and optional helper mode answer the same full-span byte Boolean query.

| September 16 saved mode | Preparation, us | First query, us | Warm foreground query, us | Process CPU/query through final idle, us |
|---|---:|---:|---:|---:|
| native | 30.425 | 332659.208 | 302452.066 | 301774.0638 |
| Progress, 2 total workers | 103.626 | 468.596 | 381.174 | 631.3678 |
| Progress, 4 total workers | 166.741 | 268.955 | 202.565 | 420.9638 |
| Same helper proof, synchronous | 53.741 | 252.800 | 215.064 | 225.4061 |

`progress-selected-results.json` contains these exact four original aggregate rows; `progress-selected-observations.json` contains the twelve selected original process records. All three repetitions of each mode completed with `no_match`. In every Progress diagnostic query, the foreground consumed one root-rejection proof, after admission at 512 native steps. Two-worker mode admitted one helper and four-worker mode admitted three. Every five-query warm phase recorded five root-rejection publications (the `warm_idle` minus `warm_before` pool-counter delta). No native-limit rescue occurred. Native and assisted controls used the same executable and native caps, so the observed benefit is caused by available negative proofs, not a changed language or a converted resource limit.

The helper's cheap proof explains most of the improvement over native parsing: synchronous execution of the same proof is also fast and uses less total CPU than Progress. Four-worker Progress is about 1.06x faster than the synchronous scan on this particular input; two-worker Progress is slower. The data supports a selected assistance gain, not universal parallel superiority.

The source was measured on September 16, 2026 at `0f21957bcbb77e7ad2ff4a2b7cf71b64bedb37e5`; the retained implementation is `c3089443fd1594691bdc904a2ef212c1cbb80ad3`, which made Progress the assisted API default and corrected scheduling-sensitive tests. Measured policies were explicitly selected. Native caps were 2,000,000 state units and 8,000,000 steps with pruning enabled; helpers received an additional 4,000,000-step allowance. Admission required 1,024 bytes and 512 current native steps. Child affinity was CPUs 0-3, with a 3 GiB address-space cap and 15 s whole-process protocol timeout; no CPU rlimit was imposed. The machine was AMD EPYC 9V74 and GCC 13.3. Metadata and exact source/binary/receipt hashes are in `provenance.json`.

The older independently clock-calibrated `mixed_prefix_late_marker_reject_4096` case has an `a^4096 b^4096` prefix and eight alternatives requiring `cc[A-H]dd`; its final marker is `Z`. It contains 8,197 bytes. The September 13 CPU follow-up retained one binary across native and assisted modes and omitted the CPU rlimit that invalidated CPU accounting in the earlier broad assistance run.

| September 13 saved mode | Warm foreground query, us | Complete CPU/query, us | Recorded cold first total, us |
|---|---:|---:|---:|
| native | 52277.187 | 52545.3384 | 63287.059 |
| 2 total workers | 129.370 | 217.9562 | 422.156 |
| 4 total workers | 24.6936666667 | 49.3864 | 416.627 |
| 8 total workers | 45.509 | 98.8894 | 608.857 |

The older mode predates Progress policy selection and admits helpers by size. Its historical helper allowance is 1,000,000 extra steps, native caps 1,000,000 states / 4,000,000 steps, admission 1,024 bytes, affinity 0-7, 2 GiB address space and 15 s wall timeout. The receipt records source and binary hashes but no Git commit; no commit identity is invented here. Current Always policy approximates the old admission setting, but cannot inherit its measurements. Current Progress comparisons need fresh results and their own receipts.

`cpu-selected-summary.json` and `cpu-selected-observations.json` retain the late-marker case and a deliberately already-cheap rejection counterexample. In the latter, native takes 5.667 us while 2/4/8-worker assistance takes 11.663/20.158/43.746 us. Extra helpers can cost more than the work they avoid. The counterexample's original fixture remains identified in the archive; its input was not copied into this minimal set.

Warm latency excludes program construction and explicit drains. Five consecutive warm batches do not wait between queries; one final drain measures complete background CPU. Assistance counters describe a separate diagnostic query, while pool counters are cumulative snapshots. Worker admission is not an instrumented count of simultaneously executing cores. For September 16, parse/preparation/first-query components are reported separately; there is no saved whole cold aggregate in the selected record. Do not add separately summarized medians and call that a measured cold total. The September 13 `first_total_us` is a recorded initial parse + construction + first-return interval; it excludes the later explicit helper drain.

The original reports remain in `original/`; the reference harness sources in `original_sources/` show exact options and controls. Their relative links refer to the original full archive. These historical subsets are evidence, not a standalone modern driver. Fresh causal comparisons should pair the current explicit native matcher with the current `AssistedProgram`, preserve equal native budgets, record consumed proofs and helper pool deltas, allow a common multiple-CPU affinity mask, and report foreground latency plus drain-inclusive CPU. Default Auto dispatch can choose a different representation and is a separate comparison.