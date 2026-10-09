# Original practical latency graph

`practical_overview.original.png` and its SVG companion are the unchanged,
receipt-verified figure from the **11 September 2026** Crank/Marpa revisit.
They are historical measurements of the supplied Constraint Parallel Core,
not timings of the September 19 consolidated runtime or October 8 integration.

The figure shows every approximately 8 KiB practical fixture: 28 cases in seven
families, with five displayed execution modes. The y-axis is logarithmic warm
query latency in microseconds. Dots are medians of three fresh-process medians;
whiskers show the observed replicate range, not confidence intervals. Shading
marks rejection established by a native root necessary-condition proof.
Preparation, input construction, I/O and first-query costs are outside this
graph's warm measurements.

Marpa is the pinned **Libmarpa 11.0.13**, commit
`243540393a690efc2f6aca20f9d57c4099dcf646`, with Leo enabled. Its adapter supplies
individual input bytes as tokens to the same AST-derived language, without
parse-forest or tree extraction and without a Crank prefilter. This does not
measure a lexer-assisted, separately factored Marpa application.

The historical planner and parallel APIs in the legend are archived. No
practical case in this campaign actually used multiple workers: surviving
practical inputs used one task and one worker. The figure is not evidence of
parallel scaling or a current runtime API promise.

## Recreate the figure without running benchmarks

Python 3 and Matplotlib are required. The original figure used Matplotlib
3.10.8 and DejaVu Sans. The script does not install packages, build engines,
download dependencies, run recognizers, or measure new timings.

From this directory:

```sh
python3 recreate_practical_graph.py --output ./generated
```

Or invoke the script by its installed repository path; its default output is
`build/benchmark-figures` below the current working directory. The compact
JSON supplies all 140 original plotted points and their three replicate
medians, case metadata and root-proof labels. The script verifies the input
SHA-256 before plotting. The CSV contains the same measurements for inspection.
Rendered bytes may differ across plotting libraries or fonts; the original
PNG/SVG remain the reference assets.

## Provenance

The containing distribution is `Crank_Certified_Demands (6).zip`, SHA-256
`9b27a5d0a34dbea2c8256f546cfe2f2fb38d624ef12abeea5ae993619807515c`.
The original figure is `crank/marpa_revisit/results/practical_overview.png`,
SHA-256 `66191643135d73261d81266ffe54eef5602814fc1688ece41e080f6464521b51`.
Its original `results/analysis.json` records that same hash and binds the source
data and plotting script.

`recreate_practical_graph.py` contains unchanged plotting functions and constants
from `crank/marpa_revisit/analyze.py`, with a portable loader for compact data.
`provenance.json` records the original script/input hashes and extracted line
ranges. Raw process journals, full environment receipts, personal paths, large
fixture subjects and unrelated campaign data are omitted.

The original report is
`crank/research/evidence/Crank_Marpa_Revisit_Report.md`: sections on practical
documents, first-use costs and measurement protocol. Original full-campaign
reproduction is documented in `crank/marpa_revisit/README.md`; it requires the
historical engine, harness and pinned dependency. Figure recreation here is a
separate operation on recorded measurements.

Supplied first-party provenance and license status remain applicable. This
package does not assign authorship or introduce a reuse license.
