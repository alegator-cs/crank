# Building and validation

`build.json` names every active engine source, target, and C++ regression.
`tools/build.py` builds sequentially, checks compiler-generated dependencies
with hashes, and records local build receipts under the ignored `build/` folder.

```sh
python3 tools/build.py --list
python3 tools/build.py crank libcrank
python3 tools/build.py --run smoke_occurrence smoke_integer
python3 tools/test.py --quick
```

`--quick` reduces generated Python cases; it still runs every declared C++ test.
The test driver also checks the public CLI and the independent regular reference
evaluator. Use `python3 tools/test.py` for the larger generated-case set.

`CXX` selects the compiler. `EXTRA_CXXFLAGS` appends flags to the normal `-O2`
configuration. For a low-cost correctness build matching the release check:

```sh
CXX=g++ EXTRA_CXXFLAGS='-O0 -g0' python3 tools/test.py --quick
```

Optional configurations are separate from the default verification:

```sh
python3 tools/test.py --sanitize --quick
python3 tools/test.py --pcre2 --quick
```

PCRE2 requires system development headers and `libpcre2-8`. Sanitizer mode uses
ASan/UBSan. Neither command is evidence that this release was checked in that
configuration. Native SLR compilation remains optional and may decline to a
portable executor. MSVC previously crashed in unchanged constrained-template
code; Windows/MSVC is not a verified build route for this release.

Include headers from `engine/` and link `libcrank.a` with pthreads. No stable
binary ABI is promised. Affine specialization requires supported 128-bit integer
intermediates or declines cleanly.

Keep observer admission and fallback contracts intact. Language-equivalent
source views may differ in histories. Compare match status and the observations
requested by the caller, and keep ResourceLimit, cancellation and preparation
unavailability distinct from NoMatch. Performance evidence must charge
preparation and query work separately and identify exact source/binary hashes.

All canonical regression sources live in `tests/`, with supporting independent
reference headers in `tests/helpers/`. The declared test target names remain
unchanged. Optional non-suite development probes are excluded. The comparison
fixtures, historical provenance and reproducibility instructions are in
[benchmarks/](../benchmarks/README.md).

Current check outcomes and incomplete gates are recorded in
[VERIFICATION.md](VERIFICATION.md).
