# Language and observable semantics

Crank matches finite **byte strings**. A pattern compiles into a finite pattern AST; this is not an input parse tree. Calls link back to rule nodes without copying them. Matching either shares context-free requests or retains histories when future observations can distinguish them.

Full membership consumes the entire supplied span. Endpoint queries begin at one specified offset and report every successful exclusive end. Neither implicitly searches every starting position. Offsets are zero-based half-open byte intervals; empty and unset capture values differ.

## Source notation

Concatenation binds more tightly than alternation. A quantifier applies to its preceding atom or group. Empty patterns, groups and alternatives are valid. Formatting whitespace is literal; there is no free-spacing mode.

| Form | Meaning |
|---|---|
| `abc`, `a\+b` | Literal concatenation; escaped punctuation |
| `a\|b`, `(?:R)` | Alternative; noncapturing group |
| `.`, `[a-z]`, `[^a-z]` | Any byte except newline; byte set; complemented byte set |
| `R*`, `R+`, `R?` | Zero or more; one or more; optional invocation |
| `R{n}`, `R{n,}`, `R{n,m}` | Exact, lower-bounded, inclusive bounded repetition |
| `(R)`, `(?<name>R)` | Numbered and named captures |
| `\1`, `\k<name>` | Substring equality reference; direction follows source order |
| `R{#n}`, `R{#n:2,5}` | Choose and bind a repetition count, optionally bounded |
| `R{#n:2}`, `R{#n:2,}` | Exact and lower-bounded named count |
| `R{=n}` | Use the current scalar count binding |
| `(?&S)`, `(?1)`, `(?R)` | Named, numbered and whole-pattern recursive call |
| `(?(DEFINE)(?<S>R)...)` | Named callable definitions; the block consumes nothing |
| `R{=rows[1].cols}` | Use the second row iteration's local count |
| `R{=S[0].@height}` | Use the first selected rule occurrence's completed height |

Every repetition invokes the body again. So `(a|b)*` may choose `a`, then `b`, then `a`; it does not capture one choice and replay it.

Names follow `[A-Za-z_][A-Za-z0-9_]*`, unique within their capture or count namespace. Capture numbers follow opening-parenthesis order, including named DEFINE groups. Returned slot zero is the whole match; substring references cannot target it. Decimal references consume all following digits: `\12` names group 12.

Escapes include `\n`, `\r`, `\t`, `\f`, `\v`, `\a`, two-digit `\xHH`, and escaped punctuation. `\d`, `\w`, `\s` have ASCII meanings and uppercase forms complement the byte set. Classes admit a leading literal `]`; ranges need increasing single-byte endpoints. Numeric escapes inside classes are rejected: use `\xHH`. The language has no Unicode character semantics, lookaround, anchors, lazy/possessive quantifiers, or Perl/POSIX capture-priority contract. Syntactic group nesting is capped at 256.

## Scalar captures and references

Closing a capture stores its current original-input interval. A later close overwrites it. A skipped capture retains its earlier binding. Failed alternatives cannot leak assignments into surviving alternatives.

A backward reference consumes the currently bound substring; an unset target fails. A forward reference consumes a candidate substring and records an equality obligation for the target's next successful close. Source direction remains fixed across repetition: a forward reference still waits for the next close even if an older value exists. No unresolved obligation is accepted. Referring to a syntactically open self/ancestor capture or an absent group is a source error.

For example, `\k<x>(?<x>a|b)` accepts `aa` and `bb`. `(?<x>[ab]*)\k<x>` recognizes equal halves over those bytes. Equal lengths alone cannot certify equality; the validated bytes must agree.

## Counts, recursion and paths

`a{#n}b{=n}c{=n}` recognizes `a^n b^n c^n`, including epsilon. A binder chooses a fresh count on each execution; a bare use reads the current binding on entry. Binding zero differs from unset. Bare uses must name a syntactically completed binder, and a binder cannot use its own bare count inside its body. Explicit paths have separate forward semantics.

Unbounded binders and bare count-use bodies need a statically provable positive minimum consumption. Finite bounded binders and explicit path-controlled bodies may be nullable, subject to history/repetition budgets. Ordinary quantifiers can also have nullable bodies.

Calls invoke a rule expression afresh, not its last captured text. Calls may be forward, self, mutual or left recursive. Missing targets are errors. DEFINE groups are numbered but execute only when called. An unproductive cycle such as `(?<S>(?&S))` has no finite derivation and does not match epsilon.

Paths resolve source labels to typed IDs when the pattern is parsed. They can compose to arbitrary syntactic depth, such as `blocks[2].rows[1].cols`. Their runtime meaning follows occurrence ownership, not just coincident input spans.

| Path component | Meaning |
|---|---|
| Unqualified path or `/` | Whole-match root anchor |
| `./` | Current occurrence scope |
| Repeated `../` | Ascend ownership edges |
| `rows[1]` for a count | Its second iteration |
| `S[0]` for a group | Its first sibling occurrence in that scope |
| `#name`, `$name` | Explicit count/group type when namespaces collide |
| Unindexed count | Completed count value |
| `.@bytes` | Selected occurrence's byte-span length |
| `.@depth` | Active source-rule depth, known on entry |
| `.@height` | Maximum completed child-rule edge depth |

The first source invocation has depth zero. A leaf rule has height zero. Rule calls and inline captures used as rule targets create rule frames; ordinary grouping and iteration ownership do not themselves increment rule depth. Missing or mismatched path targets never become zero.

Explicit forward paths freeze their anchor and every resolved component, choose a feasible candidate integer, and retain an obligation until the required observation is available. An absent unindexed component selects its next occurrence. Closed scopes with missing targets fail. `@bytes`, count and height become available at completion; depth is available on entry. A candidate value is accepted only when the observed occurrence validates it.

For `S = a S b | epsilon`, the root invocation matching `aaabbb` has height three and its epsilon leaf has depth three. So `(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}` accepts `aaabbbccc`.

## Result contracts

Boolean membership, unique endpoints, one witness, all public outcomes and a complete trace are different queries. One witness is deterministic for an implementation but has no greedy, leftmost-first or POSIX selection promise. Different engines may choose different valid ambiguous witnesses. All-outcome enumeration deduplicates identical public outcomes while retaining distinguishable histories during matching.

A trace contains only one successful derivation: root/group/count/iteration records, parent-child ownership, source IDs, original spans, counts and depth/height. Equal spans with different observations cannot be merged blindly. `Program` caches additional occurrence compilation after its first use; copied handles share that immutable compilation. `OccurrenceProgram` also supports direct repeated queries. Scalar-only requests omit occurrence-history records when neither explicit path syntax nor trace output requires them. Explicit paths currently enable history for the entire VM query, including Boolean queries; history is not yet projected independently inside each subexpression.

The native request machine answers pure context-free membership/endpoints/one-witness queries. The same occurrence VM handles scalar references/counts and scoped histories/paths, plus trace or exhaustive-outcome requests. The old scalar functions are compatibility entry points to that VM. Automatic regular matching also falls back to the native request machine when DFA compilation exceeds its cap and no certified greedy scan applies. Retired comparison and parallel engines are preserved in Git, outside the live runtime. See [API selection](API.md).

## Incomplete work

ResourceLimit means the query remains undecided. It is not false, an empty endpoint vector, or an exhaustive partial list. A witnessed success may be returned despite an irrelevant deferred branch; incomplete rejection or exhaustive enumeration may not. Nullable recursion can produce infinitely many observable histories even when pure membership is easy. Scalar compatibility budgets now count cumulative VM allocations, so an old scalar evaluator's peak-live numeric quota is not an equivalent budget. Optional asynchronous helpers have their own allowance and do not subtract from the foreground native budget. Default progress-based admission observes only current native work; the experimental learned policy can change launch decisions across queries. Both policies require a sound certificate before rejecting.

Optional native representations use at most three attempts: current frontier acceleration, the preceding aggregate/local representation, then ordinary native execution. A tier retries only when an optional representation it used exhausts the configured allowance; its graph is released before the next tier receives a full allowance. Safe result-slot and implicit-agenda storage policies remain configured. Total charges and latency include discarded attempts; limits are per attempt, not a combined cap. This protects the specified prior/default and ordinary policies, not the best quota of every arbitrary mixture of ablation switches. See [API resource contracts](API.md). No universal linear-time bound is claimed for the native or stateful machines.
