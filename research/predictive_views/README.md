# Predictive scope and payload-view research

These prototypes are **off by default** and excluded from `build.json` and every
production translation unit. They preserve tested source from a separate
research snapshot, whose commits are not ancestors of the public repository:
`32dc412` (scope source), `1fbd919416dce6f6ffe016dd22e3c5486d08cc9f`
(scope validation), `e2a2774fdb95dd54c38a8ec0d4c8c3a724542c52`
(payload-view source), and `8655b7a58b12f7fdeb936d82b36cbc3e45c952a3`
(payload-view receipts). The two headers and five C++ probes are unchanged;
only their location and this reproduction wrapper are new.

The experiments support research on observer admission and immutable table
reads. They establish no production speedup, constructor elimination, or
physical memory saving. For proof context, see the integration research notes
in this repository. Local build outputs and receipts go under ignored `build/`;
historical private process dumps and packaging documents are excluded.

## Behavior and limits

`predictive_scope_summary.hpp` builds a 256-bit forbidden-FOLLOW-byte mask only
after a successful ordinary certificate for the same immutable AST and completed
fact table. It accepts at most 4,096 nodes and 16,384 edges; its
external cache bounds summary attempts at 128. Unavailable evidence returns
`nullopt`. The mask is a semantic shadow check, not resource admission, observer
identity, EOF permission, or an execution result. Ordinary constructors and
certificates remain authoritative. The external summary storage and its reduced
operation counter are outside production accounting and cannot replace it.

`predictive_payload_view.hpp` first asks the **original cache** for the requested
observer certificate. A failed or tombstoned request returns before any pool
lookup. Independently admitted equal tables may share read backing within one
source owner, while both original certificates and all original buffers remain
retained and fully charged. Entry/root metadata, exact-boundary identity, and
consumer identity come from the requested record. Pooling is opt-in through
`PredictivePayloadViewOwner(cache, true)`; the constructor defaults to `false`.
It changes neither native engine execution nor preparation authority. Optional
pool exhaustion/failure returns the independently admitted view.

The original AST, completed facts, control arrays, and cache must outlive the
facade and every view execution. The scope cache requires owner synchronization;
the view facade's optional hook must terminate promptly and never re-enter the
facade. These preconditions do not establish concurrent publication or arbitrary
lifetime safety. Allocation hooks in two probes replace allocation only in their
standalone executables, never in `libcrank` or production sources.

## Recorded correctness evidence

The following is historical bounded validation on 11 October 2026 with
GCC 11.4/C++20, `-O0 -g0`, enabled warnings, serial workers, a 2 GiB address-space
limit, and a 45-second timeout per check. It is not a new measurement on the
current integration tree. Counts overlap and are not independent samples.

| Probe | Recorded result | Scope |
| --- | ---: | --- |
| `transfer_scope` | 19,488 header constructions | 36 fixtures, singleton/selected-subset scopes, payload equality, trimmed-root keys, EOF distinctions, NUL/high-bit relabelings, bounded unavailability |
| `runtime_probe --check-only` | 1,939 checks | 132 oracle queries, 871 native queries, ordinary charge/cache behavior, 120 quota cases, distinct ResourceLimit and precancellation |
| `slot_authority_probe` | 6,913 checks | 48 calibrated allocation-fault cases, six actual 8 MiB retention histories, permanent declines, 162 warm pairs |
| `slot_execution_probe` | 1,794 checks | 1,080 native queries, 17 oracle queries, fallback/NoMatch/ResourceLimit, two precancellations and one controlled synchronous cancellation |
| `payload_view_probe` | 4,499 checks | 12 cache cases, 144 external interpreter queries (72 Match/72 NoMatch), original identities/full charges, owner isolation, optional-stage fallback/retry |

The three slot/view probes total **13,206 checks**. Native shared-view queries
were **zero**; the external interpreter reads the facade for its 144 queries.
The native probes test unchanged production behavior alongside these prototypes.
The 800-query quota grid observed 32 retries, but its printed final retry statuses
were ResourceLimit: successful quota-retry completion remains unestablished.
No timing mode was run. The native-plus-shadow timing harness measures additional
work and cannot by itself establish an optimization benefit.

Remaining gates include maximum queue/edge boundaries, failed summary-attempt
cache-cap stress, unproductive-grammar fact solving, concurrent publication and
borrowed lifetime, asynchronous cancellation, sanitizers and a second compiler,
the full canonical suite on a proposed integration, trimmed-root **view
execution**, successful quota-retry completion, and balanced cold/warm service
measurements. Integrating this package does not satisfy these production gates.

## Reproduce against the current engine

Use an existing C++20 GCC/Clang toolchain and Python 3; no installation is needed.
The wrapper supplies the current repository's `engine/`, root, and this directory
as include paths. `transfer_scope` is header-only; the other four link the
current `libcrank.a`. Build the library and probes with matching flags.

```sh
# From the repository root; build and checks are sequential.
python3 tools/build.py libcrank
python3 research/predictive_views/run.py --build --check

# Historical low-optimization configuration (not a performance measurement).
CXX=g++ EXTRA_CXXFLAGS='-O0 -g0' python3 tools/build.py libcrank
CXX=g++ EXTRA_CXXFLAGS='-O0 -g0' python3 research/predictive_views/run.py --build --check

# Select a bounded subset, or separate compilation from correctness execution.
python3 research/predictive_views/run.py --list
python3 research/predictive_views/run.py transfer_scope payload_view_probe --build
python3 research/predictive_views/run.py transfer_scope payload_view_probe --check
```

The wrapper never installs dependencies or enables a production feature. It
uses 180-second build and 45-second check timeouts, adjustable through its help
options. On POSIX, children run on one available CPU at lowered priority with a
2 GiB address-space bound; Windows retains the timeout but lacks those POSIX
bounds. This execution bound is separate from Crank's resource policy. Use
`--library` for another matching library location and `--output` for another
output directory. Per-probe build receipts bind the binary to its source, the
prototype/current engine headers, helper headers, and linked library hash;
`--check` rejects changed inputs or binaries. Build the library through the
repository builder first so its own dependency checks also apply. MSVC is not
a verified toolchain for the existing engine.

`runtime_probe` without `--check-only` enables its original timing mode. The
wrapper always passes `--check-only`. The scope probe's separate `--timing`
mode also remains outside the wrapper. Such timings require a separately
controlled measurement plan: repeated header constructors differ from the live
cache, and temporary versus retained certificates have different lifecycle costs.
