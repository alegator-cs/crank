# Expression preparation measurement

`stage_runner.cpp` measures parsing **Crank grammar source** and fresh preparation
of the resulting program. It does not parse a regex-syntax input string with a
second grammar, scan a matching subject, or compare recognizers with Marpa.
The JSON grammar fixture is grammar source; no JSON subject is recognized here.

Build optimized baseline and modified libraries with identical compiler/flags
(no GPU, no extra workers). The Python drivers take explicit library/binary paths
and write to ignored `build/expression_prep/` directories. They need only Python's
standard library and the existing C++ toolchain; no sibling evidence directory or
private build helper is required. Example, from the modified repository in Linux/WSL
(choose an unused baseline worktree directory and an available logical CPU):

```sh
git worktree add --detach ../crank-expression-base f57ac0a1552154fc27b27f5293b2f09704e8ae1e
(cd ../crank-expression-base && CXX=g++ EXTRA_CXXFLAGS='-g0' python3 tools/build.py libcrank)
CXX=g++ EXTRA_CXXFLAGS='-g0' python3 tools/build.py libcrank
CPU=0
taskset -c "$CPU" nice -n 15 python3 benchmarks/expression_prep/paired.py \
  --baseline-lib ../crank-expression-base/build/hybrid/libcrank.a \
  --modified-lib build/hybrid/libcrank.a \
  --baseline-label f57ac0a1552154fc27b27f5293b2f09704e8ae1e \
  --modified-label "$(git rev-parse HEAD)" --out build/expression_prep/paired
taskset -c "$CPU" nice -n 15 python3 benchmarks/expression_prep/recheck.py
```

Record any uncommitted engine changes alongside the revision labels. The build
tool defaults to `-O2`; `-g0` above matches the historical measurement flags.
`paired.py` compiles one identical current `stage_runner.cpp` against each input
library, then records source/library/binary SHA-256 hashes. Compare compatible
engine interfaces. The default output directories must be empty; use `--out` for
each new campaign. `recheck.py` also accepts `--baseline-binary` and
`--modified-binary` when the paired output is elsewhere.

The baseline and modified binaries must use identical harness source, flags,
fixtures, requested repetitions and rounds. No persistent preparation cache is
used. Each `Program` receives a fresh owned source copy as its public constructor
requires. Every operation constructs and destroys its result inside the timing
batch. Source fixture storage and pre-parsed AST storage stay alive outside the
component measurements; each component builds its own result anew. The extra
`ast_copy` stage isolates one deep AST copy for context. The precomputed control
map is supplied to `equivalence_quotient`, matching that API's contract.

```sh
build/expression_prep/paired/binaries/modified/stage_runner --reps 16 --rounds 3 --max-cpu-ms 30000 > raw.jsonl
build/expression_prep/paired/binaries/modified/stage_runner --fixture literal_1024 --stage parse_source --reps 512 --rounds 7 > parse.jsonl
build/expression_prep/paired/binaries/modified/stage_runner --list > inventory.jsonl
```

`--plan repetitions.tsv` accepts whitespace-separated fixture, stage and
repetition count rows. The paired driver calibrates baseline batches toward
10 ms CPU, caps repetitions at 100,000, and freezes the same plan for both
binaries. It then executes eight serial AB/BA pairs with balanced run order.
Run the paired command above under a coordinated one-core
`nice`/`taskset` slot after building both archives. Each child has a 240-second
operational wall cap and a 60-second measured process CPU budget. Preflight and
allocation work are explicit and excluded from timed batches. Keep all raw
calibration and pair files; tiny declined-plan attempts may remain below the
target batch duration at the repetition cap.

`--pairs`, `--target-batch-ms` and `--max-reps` permit a bounded driver smoke run
(for example `--pairs 1 --target-batch-ms 0.1 --max-reps 64` with a separate
`--out`). Such a run validates the pipeline and is too small for performance
conclusions. The targeted recheck retains all its selected regressions and the
positive control, reports separately, and never replaces or pools primary rows.

`historical-summary.json` preserves all primary fixture/stage median CPU rows,
declines, aggregate weights and the separate targeted results from the original
12 October campaign. Its baseline engine was `f57ac0a` and its expression change
was `0545f65`; these are historical results on the isolated expression tree.
They are **not measurements of the later integrated tree**, and the 1.153x
parser / 1.024x preparation figures do not establish a universal gain or cold
wall latency. The full source report is [EXPRESSION_PREPARATION.md](../../docs/EXPRESSION_PREPARATION.md).

`--reps` is capped at 100,000 and `--rounds` at 99. The CPU budget is checked
before each batch; the current bounded batch may finish past it. An exceeded
budget exits nonzero with the already-emitted measurements intact. Use small
repetition counts for the full matrix, then raise them for individually selected
cheap stages. A single untimed fresh-construction warmup/preflight is explicit.
A declined constructor is emitted as `kind=declined`, with its exception text,
instead of timing a compiler-limit exception as successful preparation.

Timing samples report `CLOCK_PROCESS_CPUTIME_ID` nanoseconds on Linux, one clock
pair per batch, with batch steady-clock wall time as a secondary load indicator.
CPU timing helps reduce scheduler noise on a loaded laptop; it does not establish
clean end-user wall latency, and cache/frequency/load variation still matters.
Alternate baseline/modified run order in serial pairs; retain every JSONL file.
Compare medians of per-operation CPU time per fixture and stage, report both
improvements and regressions, and avoid silently dropping compiler declines.

The component stages overlap. In particular:

* `parse_source` scans source bytes, creates AST nodes, resolves calls/references
  and count paths, validates count bodies and finalizes the AST. The parser has
  no separate lexer-token API. For recursive sources with named count body
  checks, its internal `OccurrenceAnalysis` work is included; ordinary and
  count-free recursive sources do not run that analysis during parsing.
* `occurrence_analysis` repeats that arithmetic/byte fixed-point analysis on an
  already-parsed AST. It isolates analysis cost, but cannot be subtracted from
  parse timing as a precise exclusive attribution.
* `length_filter_integer_on/off` prepares necessary length conditions, including
  the optional shared-count integer row. Plan constructors include their own
  regular fragment work where applicable. A declined optional plan is a normal
  result, so its attempted-construction cost is retained.
* `root_regular_program` prepares the root DFA. `root_regular_fragment` includes
  its DFA and structural direct-scan/boundary preparation. Their sum double-counts
  work. Recursive roots are skipped by their regular eligibility contract.
* `transparent_control`, `equivalence_quotient` and `boolean_source_view` expose
  normalization/interning-related detail APIs independently. `ast_machine_program`
  includes checked AST ownership, analysis, control/equivalence, source views and
  the regional/native preparation portfolio. It is an aggregate, not another
  additive stage.
* `occurrence_program` includes AST ownership, shared AstMachine preparation when
  admitted, immutable analysis and effect certificates. `program_integer_on/off`
  is the complete public source preparation. `assisted_program_one_worker`
  includes its own preparation/pool setup with a caller-only worker setting.

Compile a separate binary with `-DCRANK_EXPRESSION_ALLOCATIONS` and use
`--allocations` for allocation profiles. Primary timing binaries use the
standard allocator without replacement operators; `paired.py` builds both
variants and uses only the standard variant for calibration and paired timing.

The optional allocation pass runs one separate untimed operation with global
`operator new`/`new[]` counters enabled (including aligned allocations on Linux).
It reports allocation calls and requested bytes, not peak/live/retained memory;
direct C `malloc`, external libraries and allocator overhead are outside it.
Counters are thread-local. Allocation-profile binary timings are retained in
their raw file but are excluded from the timing analysis.

Fixture records preserve source strings, AST node counts and a deterministic
AST fingerprint. Malformed records preserve exact diagnostic strings and time
both parse failure and public Program failure. These are supporting evidence;
they do not replace semantic equivalence and ownership/resource-limit tests.

There are 20 valid fixtures: tiny/common regular patterns, 64/1024-byte literals,
64 nested captures, nullable/mutual recursion, acyclic calls, nested nullable
repetitions, counts, backreferences, affine shapes, JSON grammar source, 128
alternatives, a 256-node character-class sequence, a 32-branch recursive
grammar, an admitted integer-row pattern and a recursive count body requiring
analysis during parsing. Seven malformed fixtures cover unclosed grouping,
reversed ranges, missing calls/references, repeat bounds, an unproductive count
body and the parser nesting cap.
