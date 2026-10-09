# Comparison adapters

These first-party wrappers support the full-span byte Boolean comparison
campaigns in [benchmarks](../benchmarks/README.md). They are separate from
Crank's production matcher and do not change `engine/external_regular.*`.

- `marpa_adapter.cpp/.hpp`: recovered direct Crank-AST-to-BNF byte-token
  adapter. `comparison::MarpaProgram` retains its original API, sequential
  instance calls, and explicit Leo option; the benchmark enables Leo.
- `pcre2_adapter.cpp/.hpp`: `comparison::PCRE2Program`, prepared once with
  interpreter or JIT mode, full-span compile anchors, UTF/UCP disabled,
  interpreter/JIT error separation, and the original match/depth/heap/JIT-stack
  caps. Only `PCRE2_ERROR_NOMATCH` is a negative match.
- `hyperscan_adapter.cpp/.hpp`: `comparison::HyperscanProgram`, prepared once
  with block-mode `\A(?:pattern)\z` anchors, SINGLEMATCH/ALLOWEMPTY flags, owned
  reusable scratch, and a terminating Boolean callback. Compile failures and
  scan limits/errors remain separate from NoMatch.
- `adapter_error.hpp`: shared `comparison::AdapterFailure` carrying an
  explicit status (`unsupported`, `resource_limit`, `incorrect`, or `error`).
  Allocation failures remain exceptions; the benchmark maps them to limits.

PCRE2 and Hyperscan objects are noncopyable and must be used sequentially.
Their `prepare`/`full_match`/`version` interface owns the library resources and
uses RAII cleanup, including when preparation fails. Optional adapters are
compiled only when their pinned dependency is available. No third-party
library source or binary is bundled here.

See [dependency pins](../benchmarks/DEPENDENCIES.json) and
[manual builds](../benchmarks/DEPENDENCIES.md). The original Marpa source was
recovered from `crank/marpa_comparison/` in the historical source archive; its
active location is now this directory. The header's AST include is the only
change to that recovered adapter.

The new benchmark includes the external wrapper call and its prepared-resource
guard in per-query timings. These measurements do not promise instruction-level
or latency equivalence with the archived monolithic runners.
