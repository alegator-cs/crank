# Private culling beside an authoritative matcher

Historical document references shown as plain text refer to artifacts outside this source export.

The native request/return machine remains responsible for recognition. Optional
helpers search for impossibilities and publish small, permanent certificates.
The foreground uses a certificate if it is already available; otherwise it
continues its ordinary computation. It never joins a helper before answering.

This replaces the synchronization pattern of the earlier shared-span experiment
for this particular acceleration path. The span graph is preserved on
`archive/pre-consolidation-2026-09-16` for historical comparison and substitution proofs. General `Program` dispatch is
unchanged: `AssistedProgram` is an explicit pure-CFG Boolean API, including recursive
rules, with no capture/history result or runtime reference/count effects.

## Admission policies

The current policy experiments share this same proof and ownership protocol.
`Progress` is the default; the history-based policy remains experimental.
`Always` attempts admission for every eligible query. `Selective` additionally
uses exact proof-capability gates and bounded per-size feedback; distribution
shifts can defeat its history-based predictions. `Progress` retains the static
gates but attempts admission only after current native work crosses a threshold
(default 512 steps, observed at the next 64-step poll). It allocates no job and
copies no borrowed input before that attempt. The original native graph and
quotas continue unchanged. Ordinary unassisted execution has no deferred callback.
See the policy experiment
and current measurements for the costs,
loss cases and default recommendation. The bit semantics below are independent
of the admission policy.

## What the bit proves

For a fixed immutable input `x`, let `R(n,x)` be all half-open spans `[i,j)` on
which AST node `n` can match, including empty spans. A helper may set node bit `n`
only after proving `R(n,x)` is empty. Bits begin at zero and only change to one.

| Observation | Permitted conclusion |
|---|---|
| Node bit is zero | Unknown; run the native request normally |
| Node bit is one | Every request for that node on this input is impossible |
| Whole-input rejection flag is one | The root cannot match `[0, x.size())` |
| Helper stopped, ran out of work, or failed allocation | Its unfinished conclusions remain unknown |

The whole-input flag is separate. A recursive root can match an interior span
even when it cannot match the entire input. Confusing these two certificates
would make pruning unsound.

These bits are stronger, coarser facts than arbitrary derivation exclusions.
This implementation does not allocate a shared bit for every possible span or
continuation. A future exact-span table must include node, start, end-demand
mode, and any observed context in its identity, with a stable lifetime. An
incomplete search for that key still cannot publish a negative conclusion.

## Why relaxed atomics suffice

Each bit is the complete message. There is no mutable payload whose visibility
depends on observing the bit. Helpers own their graphs, scan state, and worklists.
`AtomicExclusions` uses a fixed array of atomic 64-bit words and relaxed
`fetch_or`; multiple publishers cannot overwrite one another's set bits.
Publication batches up to 64 node certificates in one operation.

A stale zero causes extra native work. A one is always backed by a completed
sound proof. The algorithm permits stale observations without introducing C++
data races. Ordinary concurrent reads and writes to a
non-atomic bitset would have undefined behavior. Relaxed atomics guarantee
indivisible accesses and per-object modification order, without ordering other
memory. See the [C++ atomic ordering specification](https://eel.is/c++draft/atomics.order).

There are still synchronization points for ownership: a mutex and condition
variable transfer a job to a worker, and shared ownership retains the immutable
input and compilation. Completion accounting uses release/acquire. No mutex
protects the native agenda, memo table, or individual derivations because helpers
never access those mutable objects. Actual lock-free implementation of atomic
word operations is platform-dependent; the tested x86-64 build supports them.

## A worked exclusion

Consider `(abc(def)*)z` on `abcz`. The helper's literal scan finds `abc` and `z`
but no `def`. After the scan completes, `def` is impossible on every input span.
The repeated body can be excluded, while `def*` remains productive through
epsilon. The native machine can still accept `abcz`.

With `(abc(def)+)z`, absence propagates through the positive repetition and
concatenation, proving whole-input rejection. For an alternation, all alternatives
must be impossible before its parent becomes impossible. Recursive calls use
the same private productivity fixed point: start with grounded live nodes,
propagate every positive consequence, and only then interpret remaining nodes
as dead. Cancelling that fixed point halfway must not publish its provisional
zeros as exclusions.

The helper also checks conservative grammar-wide byte and adjacent-byte sets.
For `(abc)*(def)*`, `defabc` contains a forbidden `fa` transition, so a helper
can reject as soon as it sees that pair. These sets are necessary conditions;
globally allowed pairs can still occur in the wrong grammatical context.

## The private work

`PrivateCuller` compiles bounded immutable information: reachable rules, parent
links, byte/pair overapproximations, and an Aho-style automaton for selected
maximal fixed strings. Compilation can decline without affecting recognition.

One helper scans the full input for literal presence and then performs the
productivity closure. It stores presence flags, not vectors of occurrence spans.
Private successor links retire a literal's output after its first occurrence,
avoiding repeated processing of the same presence fact on dense overlapping
inputs. Memory depends on the bounded grammar and distinct literals, rather than
the number of literal hits.

Additional helpers scan later input partitions for forbidden bytes/pairs. They
check the pair crossing the left boundary of each partition. Their work overlaps
the full presence scan intentionally: a late rejection can reach the foreground
before the full scanner reaches it. They do not independently infer literal
absence from an incomplete shard.

## Admission, retirement, and ownership

Workers are persistent and created with an eligible program. `workers` includes
the calling thread. A query attempts one nonblocking mutex acquisition and
uses whatever idle workers are available. A busy pool causes immediate native
fallback. There is no waiting queue of obsolete speculative jobs.

Each admitted query has its own immutable input, exclusion table, and stop flag.
When the foreground finishes or throws, it sets that flag. Helpers poll it, and
the root rejection flag, at most every 64 charged operations, including inner
automaton and closure loops. Old work therefore stops without completing its
scan. A worker releases its private buffers and becomes available for a new job.

This is cooperative cancellation of disposable work. Asynchronous `pthread_cancel`
is unsuitable for code interrupted inside arbitrary C++ allocation, containers,
or destruction: POSIX does not require those operations to be async-cancel-safe.
See [POSIX cancellation requirements](https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html)
and [cancellation type](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_setcancelstate.html).
The 64-operation bound is not a wall-time guarantee: descheduling, allocation,
zero-initialization, and reclamation can take longer. No thread is killed while
it owns process runtime state.

The borrowed `string_view` overload copies an admitted input at helper admission
(after the threshold under Progress). This makes returning before the helper safe even if the caller
immediately mutates or frees its buffer. The owned overload retains a
`shared_ptr<const string>` instead. Its contract requires the underlying bytes
to stay immutable; a separate mutable alias must not modify them. Copies of a
program share the pool, but concurrent queries are not serialized. A query whose
optional admission fails still retains its input owner through native fallback.

`wait_idle()` and destruction of the last program handle sharing a pool are
explicit reclamation barriers. Normal
`full_match` does not invoke either. Creating and destroying a program per input
would reintroduce thread startup and joining into the end-to-end operation.
Private culler buffers are destroyed by helpers. Final shared job/input ownership
may be released by the foreground if helpers have already finished, so not all
reclamation is guaranteed to happen away from the query thread.

## Budgets and counters

Defaults are two total workers, a 1,024-byte admission threshold, a 64 MiB retained
input cap, and one million extra helper work steps divided across admitted
helpers. Every query retains its full native state/work allowance. Small inputs,
oversized inputs, unavailable analysis, and zero helper budgets take the original
native specialization, as do busy-pool fallbacks.

Native assisted execution checks node bits before demand creation and before
running excluded task owners, and polls the whole-root flag every 64 charged
native operations. Empty certificates preserve native work/state charging. The
unassisted template specialization has no assistance fields or atomic polling.

An already-published root proof can resolve a native `ResourceLimit` as false.
`native_limit_rescued` identifies this case, and `matching.limit_reached` remains
true as a diagnostic. Without such a proof, limits remain unresolved exceptions.
Helpers never turn their own exhaustion into a mismatch.

`AssistedStatistics` describes the foreground query and certificates it consumed.
Pool counters are cumulative; helper steps and bytes are added after each task
finishes. An immediate snapshot can therefore be incomplete and can combine
observations from different instants. An idle snapshot without concurrent
submitters provides completed totals. `max_stop_observation_ns` includes possible
scheduler delay. `max_cleanup_ns` covers observed cancellation through destruction
of the culler's private buffers, excluding later shared job/input reclamation
and pool bookkeeping.

## Use and evaluate

```cpp
#include "assisted_matcher.hpp"
crank::AssistanceOptions options;
options.workers = 4;
crank::AssistedProgram program(crank::parse("(?<S>a(?&S)b|)"), options);
auto input = std::make_shared<const std::string>("aaabbb");
bool matched = program.full_match(input);  // tiny input uses native fallback
```

```sh
python3 tools/build.py crank_assisted
./build/hybrid/crank_assisted --threads 4 --min-helper-bytes 0 --drain -- \
  '(abc(def)*)z' abcz
```

The CLI reports return latency separately from an optional drain. Process exit
still destroys the program and joins its workers. The benchmark harness
measures first use, steady-state queries without per-query waits, helper tails,
and total process CPU through the final drain. See its results
before choosing worker counts.

No software protocol can guarantee that these helpers never slow down native
execution on shared hardware. Dispatch, atomic reads, input copying, allocator
contention, caches, bandwidth, and CPU scheduling have real costs. The guarantee
here is narrower and testable: no waiting for optional work, no native-budget
deduction, no shared mutable parse graph, and no change to completed recognition
semantics. Assistance remains optional so measurements can guide admission.
