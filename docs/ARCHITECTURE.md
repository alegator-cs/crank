# Architecture and invariants

Historical document references shown as plain text refer to artifacts outside this source export.

Crank keeps one finite AST for the pattern. Structural children describe the expression; recursive calls point back to existing nodes. Matching does not copy or grow the AST. Input positions, caller continuations and observable environments carry the dynamic state.

## Compile once, choose the required observations

`Program` parses names, recursive targets and typed occurrence paths, then compiles necessary arithmetic facts and any eligible exact specialization. Immutable compiled ownership supports copied handles and concurrent queries with private matching state. Native/history programs needed by a later result projection are initialized once and retained.

| Query or certified language shape | Execution |
|---|---|
| Regular Boolean matching and endpoints | Direct byte scan, certified greedy scan, DFA, or optional PCRE2 fragment |
| Shared-count, equality or affine shape | Exact specialized validator; unsupported shapes use general execution |
| Pure recursive Boolean membership | Native demand execution; certified roots use compact predictive continuations |
| Pure recursive endpoints or one witness | Native AST requests and returns with the requested observations |
| Runtime references/counts, scalar captures, exhaustive outcomes | Unified continuation VM |
| Explicit occurrence paths or a full trace | The same VM retaining occurrence histories |
| Optional pure-CFG rejection assistance | Native execution with private helpers publishing impossibility bits |

Acyclic pure executed call graphs are certified regular even when source subroutines are not inlined. Unexecuted declarations are epsilon. Call-site NFA fragments preserve distinct return continuations, without constructing an expanded AST. Cyclic calls remain native, with a bounded predictive certificate available for closed Boolean roots. DFA construction is bounded by NFA/DFA states and transparent expansion depth. Automatic execution falls back to native AST recognition when compilation exceeds its budget; explicitly forcing an unavailable DFA reports a resource limit. `States` remains a compatibility selector for scalar queries, not a second stateful implementation. Legacy `match_*` functions delegate to `OccurrenceProgram`.

## Proof ordering and call-transparent regular scans

For `Program::full_match`, explicit backend validation precedes negative proofs.
With optimization enabled, cheap root bounds/congruences precede the bounded
integer-row proof; Auto then checks any compiled regular boundary certificate
before entering an exact scanner, DFA or general matcher. Every proof may
decline. Passing a necessary condition never establishes membership.

A memoized bounded view of the pure executed call DAG exposes both boundary
facts and Cartesian-period scans. It follows resolved calls and capture aliases,
erases proven epsilon shells, and retains multi-byte alternative correlation.
The called form of a fixed record can therefore use the same scanner as its
flattened form. The DFA is still compiled once under the existing caps; optional
view-analysis exhaustion retains it. A complete Boolean span can test its first
and last two bytes in constant work. Prefix, endpoint and earliest-failure APIs
never apply this whole-subject suffix test.

`LengthFilter` may also retain a necessary row
`length = offset + a*x + b*y` for two independent fixed-width repetition sites,
with their source count bounds. The existing bounded integer solver is queried
only for infeasibility. Unsupported shapes, overflowing extraction and exhausted
arithmetic continue normally. Its allowance is separate from native work.
Calls and general variable length unions are outside this narrow certificate.
There is no external LP/MILP solver in the dispatcher.

## Arithmetic and byte facts reject demands

Every native node carries productivity, minimum length, one congruence, a union of residues modulo 64, and a 64-bit set of admitted actual lengths below 64. The last set distinguishes short gaps that modular wraparound cannot: admitting length 65 modulo 64 does not alone admit actual length 1. Concatenation composes these sets by bounded sum; alternation takes their union; repetition and recursive dependencies reach a completed fixed point. Unknown runtime effects widen the summary.

FIRST/LAST/nullability and hashed ordered first/last byte-pair masks add byte conditions. Pair-mask collisions admit extra work but cannot reject a valid input. Concatenation suffixes summarize the whole remaining expression. Exact-end requests may test its exact required length; remaining input capacity is not an exact-end claim.

These summaries are necessary conditions, not replacement recognizers. Every genuine derivation satisfies them. A failed condition proves impossibility; passing only permits matching. Overflow saturates or declines a certificate. Incomplete fixed-point approximations are never published as negative proofs.

The optional synchronous `InputConstraints` index records complete overlapping literal starts, fixed-word runs and input-relative node viability. It can reject a root or answer selected exact-span questions. Declined/incomplete indexes mean unknown. The ordinary dispatcher does not require this scan for every query; benchmark wrappers use it to compare synchronous preparation with asynchronous presence-only proofs.

Independent count/equality/affine validators retain inexpensive integer reasoning: fixed-width equations, bounds, GCD/congruence tests and supported exact split constraints. The narrow height certificate covers a fixed-width recursive spine and its height-controlled suffix. No general LP or MILP solver is part of the runtime.

## Native requests, returns and recombination

A pure request identifies `(node, begin, exact end or unrestricted)`. It owns successful endpoints and return links describing the callers that consume them. Re-entering an existing request subscribes to that work. New links replay existing endpoints; new endpoints reach existing links. Left recursion becomes a finite sharing relation instead of C++ recursion or duplicated syntax.

Control collapse bypasses unobserved wrappers. Transparent return compression follows chains whose intermediate distinctions cannot affect the caller. Ordered Boolean/endpoint execution closes all work at an input position before advancing; only then may it freeze subscriber sets for older requests. Nullable work at the current position must still be allowed to add callers.

The same observation boundary permits completion coalescing. For a pure caller, two child derivations that deliver the same endpoint to the same continuation need only schedule that continuation once. Small concatenation boundaries and repetition counters use bitsets for this test. Frozen subscriber lists group consecutive links by caller bitmap word; a covered word skips a batch of redundant notifications. Surviving notifications keep their original order. Endpoint insertion also exploits the ordered scheduler. `coalesce_returns=false` provides a diagnostic ablation.

Pure terminal consumers now perform the byte/epsilon test and schedule their continuation directly, avoiding a separate request, result and subscription chain. A bounded stable structural bisimulation can also share equivalent pure node languages. Tentative partitions are discarded on compilation-budget exhaustion. `inline_terminals=false` and `merge_equivalent_nodes=false` isolate these changes.

These reductions do not change native witness selection or merge capture histories. The witness path preserves one finite proof. Pure matching still has grammars that require substantial work and memory; sharing does not establish universal linear complexity.

Dense transparent subscriptions can now promote to labelled caller-word sets.
Their fresh completion deltas enter the agenda as words, preserving the exact
caller association. Callers with the same remaining concatenation node,
boundary, demanded end and current position can ask a child question once for
their whole correlated set. A general union of starts would be unsound if their
consumers differed; this grouping retains those distinctions.

Pure acyclic subtrees, consecutive regular concat children, and regular members
of mixed alternatives can execute through the existing DFA locally. A persistent
cursor crosses each ordered byte frontier and publishes ordinary native
endpoints. Recursion, subscribers and exact demands stay in the native graph.
Construction caps and optional-compilation decline retain the ordinary route.

After a frontier closes, its request and return lookup keys cannot be queried
again. Clearing those indices avoids retaining historical hash populations;
stable graph records remain available for later completions. Optional summary
exhaustion releases its graph before retrying with a full allowance. The current
tiers disable selected source views and executor/representation families
monotonically when used, reporting discarded charges separately. The conservative
maximum is ten retries and eleven attempts, each with its full allowance; the
precise contract is described in [API limits](API.md).
See the worked relation architecture
for the invariants, examples, controls and observation boundaries.

## Frontier representations

Closed internal requests can retain only their latest result after their start
frontier closes; accepting roots retain exported endpoints. The request arena
also encodes each pending Down action, with task watermarks preserving exact
FIFO interleaving. These storage quotients preserve the logical schedule.

Nullable FIRST-byte exclusion can prove an epsilon-only unrestricted relation.
If the exact-end demand permits that endpoint, the continuation resumes directly;
otherwise the demanded relation is empty. Neither case needs a request.
Frozen dense caller rows can instead use bounded run/prefix-union certificates
for bits already published at an endpoint. This removes repeated aggregate word
scans while retaining each caller and exact-end correlation. Neither operation
constructs a global input-span chart. Sparse and witness paths remain distinct.

The consumer-frontier architecture
explains these earlier invariants, admission caps and same-binary controls.
The current certified-demand architecture
adds worked recursive and endpoint examples and the current retry contract. Optional
representations have monotone full-allowance retry tiers, described precisely
in [API limits](API.md); total discarded work is not hidden.

## One determined action can replace an interior request graph

A bounded `PredictiveRegion` certificate permits a closed full-root Boolean
demand to execute the existing AST with `(node, child boundary or repeat count)`
continuations. It requires reachable recursion and an acyclic graph of actions
that can occur before consuming a byte. This excludes left recursion hidden
behind nullable prefixes. Byte FIRST sets choose disjoint alternatives;
nullable alternatives and variable repetitions must also have no FIRST/FOLLOW
conflict. Repeated bodies are nonnullable. The original root's only external
symbol is EOF. Conservative conflicts decline to ordinary native recognition.

For `S = a S b | epsilon` on `aaabbb`, each `a` leaves a pending trailing `b`;
the first `b` selects epsilon, then three pending effects consume `bbb`. The
accepting boundary remains a native request, but there are no internal request
keys, subscriber lists, endpoint records or agenda messages. This is an exact
physical representation of one certified demand inside native execution.

Identical pending `(node, control)` frames can form a run with a multiplicity.
The run preserves every repeated effect; it drops only unobserved invocation
identity. Direct terminal prefixes avoid transient frames that would obscure
run equality. When the complete remaining effect is one byte class, a run
executes in batches of at most 64 bytes with charged work and cooperative
polling. Distinct controls split the run; alternating return shapes may not
fold at all. Logical pushes/depth and physical run allocations/depth have
separate counters. This is not constant-space parsing of all certified grammars.

Endpoint enumeration, witnesses, arbitrary nonroot entries, multi-entry OR and
histories do not use the closed-root certificate. In particular, S on
`aaabbb!` has endpoints `{0,6}`: both epsilon and a nonempty prefix must survive.
Existing native endpoint execution retains them. Optional predictive exhaustion
retries the preceding representation with its full original allowance.

## Consumer proofs can omit local endpoint publication

An accepting local DFA cursor may still advance on the actual next byte. Before
publishing that proper-prefix endpoint, a bounded guard asks whether any frozen
consumer can use it. Remaining concatenation FIRST sets are composed through
nullable children; epsilon-only tails follow existing return edges. Exact ends
and accepting/exported boundaries remain explicit observers. Only a request
with `begin < current frontier` has a frozen subscriber list. Start-frontier
requests, repetition controls, dense subscribers and exhausted proof budgets
admit conservatively, preserving late nullable replay.

For an identifier followed by a nullable operator tail and a required semicolon,
interior identifier endpoints can be valid lexical matches yet impossible for
every caller. Their publication and downstream continuation work can disappear
while the DFA reads exactly the same bytes. A second caller able to use an
endpoint preserves it: the proof takes the union of all consumers and never
narrows a shared request globally.

Each proof inspects at most 64 requests/suffix children. A 64-byte query-local
fuel table, hashed by producer, boundary and next byte, suppresses repeated
unproductive probes after 16 failures per slot; an exclusion replenishes fuel.
It stores admission history, not recognition facts. Hash collisions or a later
context shift can miss useful proofs without changing answers. Final endpoints
whose cursor cannot advance bypass this optional search. These are synchronous
foreground proofs; their cost is charged and has its own full-allowance fallback
tier. Witness and stateful paths remain unchanged.

## One VM, three observation modes

The continuation VM uses heap-resident tasks and branch environments for captures, named counts and pending forward obligations. A forward reference constrains the target's next close; it does not read a previous iteration's capture. Bare references/counts use latest bindings. Explicit paths select a particular occurrence.

For scalar results without paths, the VM omits group/count/iteration records entirely. Its memo key contains the input position, active call context, canonical continuation and scalar environment, including pending obligations. Equal positions alone are insufficient. For Boolean and endpoint queries it additionally removes capture/count slots absent from the global reference/count-use read set. Full scalar outcomes retain every reported slot.

An unbounded repetition count becomes irrelevant after its lower bound is met when no observer can read it. Only the memo identity saturates that count; raw continuation frames retain unfolding-budget information. Nullable cycles can then close over a finite set of scalar configurations. Observed counts stay distinct. For a nullable write-only scalar body, however, the entire transition relation stabilizes above `remaining_bytes + 2*live_capture_slots + 1`. Retaining consuming iterations, first writes for pending obligations, last writes for outputs and one epsilon iteration proves contraction and arbitrary padding. The VM can clamp eligible huge bounds while preserving all scalar outcomes. Calls, references inside the body, count effects and history exclude this certificate. Merely leaving today's environment unchanged on one epsilon edge is still insufficient.

Traces and explicit paths retain full occurrence records, parent ownership and obligations. Depth is available on rule entry; height is completed information. Actual path nodes determine whether histories are necessary, including when advisory AST flags are stale. The VM does not yet project arbitrary histories locally inside an otherwise path-sensitive query.

The old scalar budget measured peak retained units; the VM measures cumulative frame/environment allocations. Numerical limits and diagnostic counts consequently differ after consolidation. The write-only saturation bound recovers some large bounded scalar cases lost during consolidation. Other read/write or history-sensitive nullable repetitions can still hit a repetition limit where the removed scalar evaluator used whole-layer periodicity to skip work. Such exhaustion remains unresolved, never a negative answer. A witnessed Boolean/first result may return despite another deferred branch; exhaustive results must report unexplored possibilities.

## Optional helpers search for negative proofs

`AssistedProgram` keeps native execution authoritative. Helpers own mutable scan/productivity state and share immutable grammar/input data. Their messages are monotone atomic bits: a node bit proves that node matches no span anywhere in this input; a separate root bit proves the whole input cannot match. The distinction prevents a failed whole-input demand from poisoning a valid recursive subspan.

Literal absence, byte/alphabet/adjacency checks and completed productivity propagation can publish these proofs. A stale zero means unknown and only permits extra work. Relaxed atomics suffice for these self-contained messages; ordinary concurrent non-atomic writes would still be undefined C++. No helper edits the foreground request/return graph.

Admission is optional and nonblocking. The pool never queues a backlog behind busy workers. Selective policy excludes helper kinds that cannot supply a proof and uses bounded per-size-class backoff after queries consume no certificate. Always policy is the unconditional control. Progress policy attempts admission after a native work threshold, without restarting recognition. Advisory races can change launch decisions, never the accepted language.

Helpers poll cancellation within bounded charged work blocks and discard their private state when the foreground finishes. Query return never joins them. This bounds work between polls, not allocator latency or wall time. Asynchronous thread cancellation is not used. Borrowed inputs are copied only when admitted; an owned immutable input can instead be retained. `wait_idle()` and program destruction are explicit reclamation barriers. Helper budgets are additional allowances and never subtract from native quotas.

## Active comparisons and archived alternatives

Leo-enabled Libmarpa remains the meaningful generalized-recognition comparator, using the pinned byte-token AST-to-BNF adapter. Optional PCRE2 accelerates certified regular fragments. Standalone PCRE2 interpreter/JIT, Rust regex and Hyperscan adapters now compare supported byte languages; they are external controls, not new runtime backends. RE2 and Hyperscan are not integrated into matching.

Earley, joined CFG task parallelism, parallel DFA composition and the bidirectional span engine are absent from the active runtime and retained on `archive/pre-consolidation-2026-09-16`. Compact native storage is preserved on `archive/compact-native`. Their old reports describe those old revisions. The standalone regular reference evaluator is an experimental correctness tool outside the common runtime library, not a fallback backend.
