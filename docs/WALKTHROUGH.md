# Worked route: syntax, facts, requests and observations

Start with `(?<S>a(?&S)b|)` on `aabb`.

1. **Parse syntax.** A capture contains an alternative: concatenate `a`, call S, `b`; or epsilon. The call holds S's existing node ID. There are no input nodes and no recursive syntax copies.
2. **Compile necessary facts.** S is productive and nullable. Its lengths are nonnegative even integers. Nonempty words start with `a` and end with `b`. `aab` fails the length condition immediately; `bbaa` fails its first-byte condition. `abab` passes those broad checks but still needs structural matching.
3. **Request the full span.** The native engine asks S to match `[0,4)`. The first `a` matches `[0,1)`. A return continuation remembers that the inner S must be followed by the final `b`.
4. **Descend with the remaining demand.** The exact final `b` gives inner S the span `[1,3)`. Its recursive branch consumes `a`, asks S for `[2,2)`, and retains the return that checks `b`.
5. **Complete and resume.** The epsilon alternative completes `[2,2)`. Return links resume the inner and outer continuations. Their `b` checks complete `[1,3)` and `[0,4)`. Root completion proves membership.
6. **Share repeated questions.** If another alternative asks for the same node, beginning and end demand, it subscribes to that request. Existing completions replay to a new caller; new completions reach existing callers. For left recursion the same mechanism reaches closure instead of expanding a C++ call stack indefinitely.

An endpoint query on `aabb!` starts with an unrestricted root end. It returns `0` and `4`. It cannot inherit the full-match assumption that the pattern ends at byte five. Exact demand is part of memo identity.

For an ordinary `(a|b)*` query, automatic dispatch can use certified regular execution. The regular compiler treats captures as transparent to membership, constructs an NFA and then DFA tables, and follows byte transitions. DFA chunk composition is now archived; current optional workers instead search for negative proofs beside native execution.

## Arithmetic is a filter, then validation

Consider `(?:a{7}|a{11})*` on 59 `a` bytes. Any match length obeys `7*x + 11*y = 59` for nonnegative integers x and y. The GCD is one and the wrapping residue closure saturates, but no such integers exist. The compiled nonwrapping length word has bit 59 clear, so native recognition rejects the exact demand before allocating a parse state. A feasible length still needs ordered byte validation: replacing a required byte with `b` does not become valid because an equation balances.

The regular/count/affine specializations follow the same discipline. For `a{#n}b{=n}c{=n}`, total length gives a single candidate count. The engine must still check three ordered runs. Arithmetic alone cannot distinguish `aabbcc` from `aabcbc`.

## When a span is not enough

For `(?:a{#cols};){#rows}b{=rows[1].cols}` on `a;aaa;aa;bbb`, the three row iterations bind local column counts 1, 3 and 2. The path selects the second iteration's 3, so the suffix needs three `b` bytes. A latest-scalar-only environment would retain 2 and answer the wrong question. The history VM retains the ownership records needed for this query.

For `(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}` on `aaabbbccc`, S's root occurrence has height three. The suffix therefore needs three `c` bytes. It is the completed source invocation's height, not the textual AST depth or the C++ stack depth.

## What optional workers can prove

For `(abc(def)*)z` on `abcz`, a completed presence scan establishes that `def` occurs nowhere. That leaf can be marked impossible; its star remains viable through zero repetitions. If the star is changed to plus, bottom-up productivity propagation can prove the whole expression impossible. Alternative nodes die only when all alternatives die; concatenations die when any required child dies. Recursive dependencies need closure, not a single tree traversal.

A helper performs this search privately and publishes irreversible absence bits. Native requests read available bits and continue without waiting. The same input must remain immutable while either participant uses it. A stale zero only misses an optimization; publication uses C++ atomics, not an unsynchronized shared bit write. Progress-based admission launches only after enough native work has occurred, so a small example may finish without a helper.

Native completion coalescing uses a different, foreground-only bit: it records that an equivalent caller/endpoint message is already scheduled. One retained real derivation still performs the completion. Several binary trees for `S -> SS | a` can therefore cause one notification when their caller consumes only an endpoint. Captures or histories that distinguish those trees use a different observation mode.

The archived joined planner demonstrated independently proven AND/OR obligations. The maintained concurrency path instead runs disposable negative-proof searches beside one authoritative recognizer. It does not require a thread per node or a shared mutable derivation graph.

These examples explain contracts, not a promise of identical witness order across historical implementations. The current architecture and benchmark report separate recognition work, optional proof work and resource limits.
