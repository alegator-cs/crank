# Expression front end and preparation — 12 October 2026

The text parser contains avoidable allocations. Removing them improves source
parsing in these fixtures, but source parsing is usually a small part of full
preparation. The large recursive fixtures spend much more CPU in the native
constructor. This work does not establish that expression parsing caused the
earlier cold Crank/Marpa difference.

The measured source is the actual Downloads export at
`f57ac0a1552154fc27b27f5293b2f09704e8ae1e`, copied into an independent repository.
The human `docs/VERIFICATION.md` edit is preserved byte-for-byte, SHA-256
`4d25f6673451b297569fc14227b408a4affad14229fae2e202004d015343884c`.
The separate `e2a2774` experiment is not merged, ported, or included in these
measurements. Originals, the other benchmark workspace and GPU work were not
modified. Local memory directories contained no files; Personal Context was
unavailable. Repository development, API, semantics and verification instructions
were read. No installs or public pushes were performed by this task.

## How preparation works

`regex_ast.cpp` is a direct byte parser: alternatives call concatenations,
concatenations call quantified atoms, and atoms handle groups, classes and
escapes. There is no separate token stream or lexer phase. Lexical scanning,
recursive parsing, AST node construction and conservative nullable/minimum
summaries are fused. Nodes are appended in structural postorder. Names and
registries own their strings. Resolution runs in the order references, calls,
occurrence paths, then count-body validation. Only sources with calls and
count-body checks construct `OccurrenceAnalysis` during parsing; its productivity
test cannot safely be omitted merely because a parser lower bound is positive.

`Program::Impl` then constructs `LengthFilter` (including the bounded optional
integer row), greedy/count/equality plans, and an affine plan when the cheaper
plans decline. Regular roots build `RegularFragment`: structural validation,
NFA/DFA construction, direct-scan and boundary certificates, and an optional
external adapter. A DFA cap retains the existing native fallback. Calls or
extended syntax prepare `OccurrenceProgram` immediately.

`OccurrenceProgram` owns an AST copy and, when admitted, an `AstMachineProgram`.
That program checks/copies the source, computes immutable occurrence facts,
collapses transparent controls, refines a bounded structural-equivalence
quotient, and prepares regular regions plus predictive/spine/rim certificates.
The facade shares its analysis and computes effects. Source-view normalization,
bounded-word/SLR choices, several regional facts and executable lowerings already
use lazy immutable caches. Their first-query cost is outside constructor timing;
this policy is unchanged. No new source or input cache was added.

## Changes

Three engine files change, with 24 added and eight removed lines:

* Allocate repeat child storage only after an actual quantifier is found; return
  singleton concatenations and alternatives without temporary child lists.
* Skip impossible equality-plan analysis in `Program` when its freshly parsed
  AST has no references. Direct `make_equality_plan(Ast)` still verifies actual
  structure, including stale convenience flags.
* Compile a complete regular root directly instead of first copying its entire
  AST. Subtrees, called/stateful sources and empty/multiple roots retain the
  existing component-copy path. Validation, caps, DFA ownership and synchronous
  construction remain intact.

Public syntax, node IDs, diagnostics, admission/fallback, ownership, cancellation,
resource budgets and requested observations are unchanged. The patch does not
defer setup or rewrite the parser or native algorithms.

## CPU evidence

GCC 11.4, C++20, `-O2 -g0 -pthread`, Linux/WSL; one logical CPU 14, nice +15.
There were 20 valid source fixtures and seven malformed sources, covering common
patterns, large concatenations/alternatives, recursive/nullable rules, integer
rows, counts, references and JSON grammar source. Eight serial AB/BA pairs used
the same baseline-calibrated repetitions, targeting 10 ms CPU per batch and
capping repetitions at 100,000. All 5,984 paired timing rows were retained.
Standard allocation functions were used for timing; allocation profiles used
separate binaries. Constructors and destruction are included. Every iteration
prepares a new object; public `Program` timing also includes its owned source
copy. Preflight, allocation profiling, component pre-parsing and fingerprints
are explicit, outside timed batches. These are fresh preparations in warmed
processes, not cold-process startup or quiet laptop wall-latency measurements.

Median paired speedups, with equal weight per family:

| Measurement | Speedup | Scope |
|---|---:|---|
| Fused source parsing | 1.153× | 20 valid sources; equal-case result 1.159× |
| Full default preparation | 1.024× | 20 valid sources; equal-case result 1.027× |
| Preparation, integer row disabled | 1.024× | Same fixtures; no runtime proof work |
| Malformed source parsing | 0.995× | Seven exact diagnostics; effectively tied |

The largest supported preparation gain is 64 nested capture groups: 1.413× in
the main campaign and 1.419× in the separate 16-pair follow-up; all 16 follow-up
pairs favored the change (range 1.374–1.584×). Parser allocations for that source
fell 285→90, total preparation allocations 456→191. For a 1,024-byte literal,
parser allocations fell 1,051→26; its full preparation changes little because
DFA construction hits its original cap and then prepares native fallback.
Requested allocation bytes are not peak or retained memory.

Selected baseline component costs, CPU µs per fresh construction:

| Component | Identifier | JSON source | Recursive 32 alternatives |
|---|---:|---:|---:|
| Fused source parse / AST build | 0.489 | 10.315 | 15.561 |
| Deep AST copy | 0.099 | 1.993 | 2.886 |
| Occurrence facts / arithmetic analysis | 0.894 | 28.290 | 138.330 |
| Length/integer filter | 0.062 | 0.060 | 0.050 |
| Structural equivalence quotient | 1.456 | 44.938 | 97.937 |
| Boolean normalized source view | 0.216 | 7.088 | 12.296 |
| Root regular fragment | 5.463 | Ineligible | Ineligible |
| Whole native constructor | 3.219 | 520.141 | 597.186 |
| Whole public preparation | 6.423 | 556.721 | 612.300 |

These independent component stages **overlap** and are not an exclusive stage
breakdown; they must not be added or subtracted to assign percentages. Native
construction is much closer to total preparation cost than source parsing for
the large recursive cases. For regular sources, the DFA/fragment construction
cost is much larger than scanning the source. The recursive count fixture is a
distinct case: parsing itself invokes analysis (16.889 µs parse, 15.258 µs
standalone analysis, 34.180 µs full preparation). Sharing that validated analysis
later may be useful, but requires careful ownership and admission changes.

Regressions remain visible. The primary default preparation paired medians were
5.8% slower for shared counts, 3.8% for the affine fixture, about 2–3% for two
recursive fixtures, with broad sample variation. Longer isolated follow-ups put
shared-count parsing/preparation at 1.016×/1.012× and affine preparation at
1.020×; these do not replace the primary observations. Identifier-fragment
construction stayed about 1.3% slower in follow-up. Unchanged regular-eligibility
code was 6.9% slower in that same comparison (all 16 pairs), demonstrating that
binary layout/load/measurement effects can exceed the small overall gain;
the cause was not isolated. The 2% aggregate is therefore tentative evidence,
not a reliable universal latency improvement. Every component's unfavorable
row is included in the raw summary.

Four identical preparation declines were recorded on both sides: root DFA and
fragment construction for the integer-row fixture and 1,024-byte literal.
They are not timed as successful construction, and public preparation still
includes attempted compilation and fallback. All completed stage checksums,
fixture identities and diagnostic strings agree. This measures Crank grammar
source, not recognizing a regex-syntax INPUT using another grammar; it makes
no new Marpa or Progress4 comparison claim.

## Verification and reproduction

`tools/test.py --quick` completed in one invocation with GCC 11.4 and
`EXTRA_CXXFLAGS='-O0 -g0'`: all 61 declared C++ targets, engine/occurrence/
capability/assisted CLI gates and 5,475 regular-reference cases passed. This
includes 2.12 million relation-oracle checks, representation fallbacks,
cancellation, histories, resource limits and the previously unverified scan
certificate target. Optional PCRE2/sanitizer configurations were not run.

The same focused tests were also linked to both `-O2` libraries. All 2,308 source
AST/diagnostic snapshots match byte-for-byte (827 accepted, 1,481 rejected),
SHA-256 `aa7b0e04157a5cb85448395e96cdf1a95c9199e58bde9db3b460aab53f6a01c5`.
Each library passed 2,299 preparation checks, including the forced old component
copy path, source lifetime, malformed public ASTs, exact compile limits,
observations and pre-cancellation.

The task's sibling `evidence/` contains canonical/focused logs, baseline source
bytes, original hashes, build receipts, raw JSONL, repetition plan, every paired
sample and both summaries. `build_expression.py baseline` reconstructs the
original three changed files and verifies every engine file against the original
manifest; `modified` builds current source. Builds are serial and each compiler
child is capped at 600 seconds. `validate_expression.py` runs the paired focused
checks. Then run `benchmarks/expression_prep/paired.py` and `recheck.py` under the
agreed one-core `nice`/`taskset` slot. The measurement README explains overlapping
components and budgets. The deliverable archive preserves this directory layout.

Further profiling should split the native constructor's certificate builders
before attempting eager/lazy changes, and examine whole-source copies in count/
equality/affine component construction. This patch leaves those broader choices
for measured follow-up. No unresolved correctness blocker was found for the
actual f57ac0a source; the separate e2a2774 integration remains outside scope.
