# Post-publication integration audit — 12 October 2026

The public repository is `alegator-cs/crank`, default branch `master`. Its
publication checkpoint is `f57ac0a1552154fc27b27f5293b2f09704e8ae1e`, verified
against the remote before this integration. The older `crank_parser` repository
is outside this audit. The integration starts from the public checkpoint in a
separate checkout; the original checkout and its uncommitted documentation are
preserved.

## Inventory

| Work | Status at publication / integration | Evidence and limits |
|---|---|---|
| Five-helper arithmetic rewrite | Already public at `f57ac0a` | Existing arithmetic dispatch and the three arithmetic-chain regressions are in the remote tree. It is not counted as a new patch. |
| Expression preparation, source commit `0545f6589a630b7488bb4e64f949da689d0940c7` | Ready production improvement, integrated | Three implementation files remove discarded child allocations, skip irrelevant equality analysis, and avoid full regular-root AST copies. Two added canonical regressions preserve diagnostics, AST structure, ownership and fallback contracts. Historical parser CPU gain is about 1.15x; the roughly 1.02x overall preparation gain is tentative. See [preparation evidence](EXPRESSION_PREPARATION.md). |
| Cached scope summary, source commits `32dc412` / `1fbd919416dce6f6ffe016dd22e3c5486d08cc9f` | Tested shadow research, retained separately | A bounded forbidden-FOLLOW-byte mask follows successful same-owner certificate admission. Ordinary constructors and certificates remain authoritative. No native constructor is removed; no production optimization is enabled. |
| Slot-authority read facade, source commit `e2a2774fdb95dd54c38a8ec0d4c8c3a724542c52`, evidence `8655b7a58b12f7fdeb936d82b36cbc3e45c952a3` | Tested opt-in research, retained separately | The original slot must admit the requested observer certificate before shared backing is consulted. Both original buffers and full logical charges remain. It is neither physical deduplication nor native integration. See [research package](../research/predictive_views/README.md). |
| Admission / accounting proofs, 9–11 October | Proof and regression-design material | Equal executable payloads do not imply equal preparation costs or interchangeable scoped admission. Exact-only reuse can lose valid consumer splits. Failed slots cannot be revived by a pool; original retention cannot be discounted. |
| Adaptive quota model, 12 October | Abstract result; no native grammar counterexample | A fixed successful attempt is quota-monotone; an adaptive controller need not be. The reported model completes at quota 5, limits at 6 and completes at 7. No production bug or repair follows without a native separator and preserved retry semantics. |
| Independent-frontends practical benchmark | Completed historical measurement, public-safe results and portable rerun tools integrated | Measured on `f57ac0a`, before the expression change. Seven phases remain separate; private/local corpus bytes and bulk logs are excluded. Public replacement fixtures are not represented as historically measured inputs. See [benchmark](../benchmarks/marpa_scanless/README.md). |

The research snapshot commits are not upstream descendants and were not blindly
cherry-picked. The research package has no production translation-unit import
and is excluded from the canonical build graph. Reported historical probe counts
are distinguished from fresh integration validation.

## Contracts and remaining research gates

Scope summaries cannot authorize observer, endpoint, resource, or EOF admission.
Shared reads require the original AST, facts, control and cache owner to outlive
execution. Existing buffers remain retained and charged even when independently
admitted equal tables share backing. Constructor elimination, physical
deduplication, successful quota-retry completion, trimmed-root execution,
concurrent publication/lifetime, asynchronous cancellation and balanced cold /
warm native measurements remain gates for a production proposal. Historical
probes alone do not satisfy them.

The proof history reports 182,640 production-header maximal-safe-scope checks
and 216,960 modeled slot-authority transitions. These counts are historical
evidence, not fresh integrated-tree test results. Their full private proof
archives are not included in this public source package. An eleven-attempt
adaptive-quota bound is a model property; final and discarded work must be
counted once without overlapping counters.

## Accounting-cap decision

The wide benchmark's eight first-query `ResourceLimit` outcomes were cumulative
state allowance exhaustion. A uniform same-input control removed foreground
accounting caps while retaining operating-system memory/time limits and helper
policy, and completed all 64 entries. This establishes a benchmark configuration
difference, not an evidence-backed production fix. Production limits, admission,
fallback, cancellation and outcome distinctions remain unchanged. See
[accounting details](ACCOUNTING_LIMITS.md).

## Preservation and measurement boundaries

The original human README prose, existing benchmark artifacts and images are
retained. The original uncommitted `docs/VERIFICATION.md` is copied verbatim,
SHA-256 `4d25f6673451b297569fc14227b408a4affad14229fae2e202004d015343884c`;
new integration verification belongs in a separate report.

No native shared-view speedup, new proof implementation, or new measurement of
replacement inputs is claimed. No C16 publication is part of this integration.
See [integration verification](INTEGRATION_VERIFICATION.md) for current test
outcomes and publication boundaries.
