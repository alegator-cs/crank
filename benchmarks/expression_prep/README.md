# Expression preparation measurement

`stage_runner.cpp` measures parsing **Crank grammar source** and fresh preparation
of the resulting program. It does not parse a regex-syntax input string with a
second grammar, scan a matching subject, or compare recognizers with Marpa.
The JSON grammar fixture is grammar source; no JSON subject is recognized here.

Build against the same optimized engine archive on each side (no GPU, no extra
workers). Example, from the repository in Linux/WSL:

```sh
g++ -std=c++20 -O2 -DNDEBUG -Iengine benchmarks/expression_prep/stage_runner.cpp \
  build/expression-baseline/libcrank.a -pthread -o build/expression-baseline/stage_runner
```

The baseline and modified binaries must use identical harness source, flags,
fixtures, requested repetitions and rounds. No persistent preparation cache is
used. Each `Program` receives a fresh owned source copy as its public constructor
requires. Every operation constructs and destroys its result inside the timing
batch. Source fixture storage and pre-parsed AST storage stay alive outside the
component measurements; each component builds its own result anew. The extra
`ast_copy` stage isolates one deep AST copy for context. The precomputed control
map is supplied to `equivalence_quotient`, matching that API's contract.

```sh
stage_runner --reps 16 --rounds 3 --max-cpu-ms 30000 > raw.jsonl
stage_runner --fixture literal_1024 --stage parse_source --reps 512 --rounds 7 > parse.jsonl
stage_runner --list > inventory.jsonl
```

`--plan repetitions.tsv` accepts whitespace-separated fixture, stage and
repetition count rows. The paired driver calibrates baseline batches toward
10 ms CPU, caps repetitions at 100,000, and freezes the same plan for both
binaries. It then executes eight serial AB/BA pairs with balanced run order.
Run `python3 benchmarks/expression_prep/paired.py` under the agreed one-core
`nice`/`taskset` slot after building both archives. Each child has a 240-second
operational wall cap and a 60-second measured process CPU budget. Preflight and
allocation work are explicit and excluded from timed batches. Keep all raw
calibration and pair files; tiny declined-plan attempts may remain below the
target batch duration at the repetition cap.

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
