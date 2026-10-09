# Manual builds of the pinned dependencies

These Bash commands target Linux/WSL and keep source, objects, and installation
under the repository's ignored `build/benchmark-deps/`. They use one build job,
no administrator privileges, and no system install. They were checked against
the pinned upstream sources and the recovered original generation/build
receipts; **these dependency builds were not executed for this release**.
Run them manually if the corresponding dependency is wanted. Existing packages
with the exact versions may be used instead. Missing prerequisites/dependencies
remain `unavailable` in the benchmark results.

Start from the Crank repository root in a fresh Bash session. Prerequisites are
Git, a C/C++ compiler, `ar`, Make, `pkg-config`, Autoconf/Automake/Libtool for
PCRE2, and CMake for Hyperscan. Libmarpa source generation also requires Perl,
CWEB's `ctangle`, and GNU Emacs with Texinfo support. Hyperscan needs Boost
headers (at least 1.57), Ragel 6.9, a Python interpreter, and x86 with SSSE3.
Use CMake 3.x, at least 3.16, for the command syntax below; the old Hyperscan
build may need compatibility adjustments with later CMake/toolchain releases.
[Hyperscan's pinned prerequisites](https://github.com/intel/hyperscan/blob/v5.4.2/doc/dev-reference/getting_started.rst).

```sh
set -e
bench_crank="$PWD"
bench_deps="$bench_crank/build/benchmark-deps"
bench_prefix="$bench_deps/prefix"
mkdir -p "$bench_deps/src" "$bench_deps/build" "$bench_prefix"
```

Use fresh source/build directory names. Clone commands and plain `mkdir`
commands below stop if their destination already contains a previous checkout
or generated build; they do not reset or delete it.

## PCRE2 10.42, 8-bit interpreter and JIT

The tag's repository needs `autogen.sh` before `configure`; a release tarball
already includes `configure`. The following enables JIT and builds only the
8-bit API used by the runner. Shared libraries go to the local prefix's `lib/`.
[PCRE2 10.42 build instructions](https://github.com/PCRE2Project/pcre2/blob/pcre2-10.42/README).

```sh
git clone --depth 1 --branch pcre2-10.42 \
  https://github.com/PCRE2Project/pcre2.git "$bench_deps/src/pcre2"
(
  cd "$bench_deps/src/pcre2"
  ./autogen.sh
)
mkdir "$bench_deps/build/pcre2"
(
  cd "$bench_deps/build/pcre2"
  "$bench_deps/src/pcre2/configure" \
    --prefix="$bench_prefix" --libdir="$bench_prefix/lib" \
    --enable-jit --disable-pcre2-16 --disable-pcre2-32 \
    CFLAGS='-O2'
  make -j1
  make -j1 check
  make -j1 install
)
```

## Hyperscan 5.4.2

Build shared libraries so ordinary `pkg-config --libs libhs` supplies the
complete linker route. The Linux fat runtime remains enabled; no AVX-512
variant is requested. Examples are omitted. Add `-DBOOST_ROOT=/path/to/boost`
if headers are outside normal search paths. CMake can select an existing Python
interpreter with `-DPYTHON_EXECUTABLE=/path/to/python`.
[Pinned CMake options](https://github.com/intel/hyperscan/blob/v5.4.2/CMakeLists.txt)
and [build procedure](https://github.com/intel/hyperscan/blob/v5.4.2/doc/dev-reference/getting_started.rst).

```sh
git clone --depth 1 --branch v5.4.2 \
  https://github.com/intel/hyperscan.git "$bench_deps/src/hyperscan"
cmake -S "$bench_deps/src/hyperscan" -B "$bench_deps/build/hyperscan" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$bench_prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON \
  -DBUILD_EXAMPLES=OFF
cmake --build "$bench_deps/build/hyperscan" --parallel 1
"$bench_deps/build/hyperscan/bin/unit-hyperscan"
cmake --install "$bench_deps/build/hyperscan"
```

## Libmarpa 11.0.13 standalone, with the private Leo APIs

Pin commit `243540393a690efc2f6aca20f9d57c4099dcf646`. Keep that checkout
unchanged for the benchmark's commit/clean-tree check; generate C sources in
a separate `git archive` copy. This follows the original standalone route,
without the upstream full PDF/documentation/distribution build.
[Pinned root Makefile](https://github.com/jeffreykegler/libmarpa/blob/243540393a690efc2f6aca20f9d57c4099dcf646/Makefile).

```sh
git clone https://github.com/jeffreykegler/libmarpa.git "$bench_deps/src/libmarpa"
marpa_source="$bench_deps/src/libmarpa"
git -C "$marpa_source" checkout --detach 243540393a690efc2f6aca20f9d57c4099dcf646
test "$(git -C "$marpa_source" rev-parse HEAD)" = 243540393a690efc2f6aca20f9d57c4099dcf646
marpa_work="$bench_deps/build/libmarpa-generated"
marpa_standalone="$bench_deps/build/libmarpa-standalone"
mkdir "$marpa_work" "$marpa_standalone"
git -C "$marpa_source" archive HEAD | tar -x -C "$marpa_work"
make -j1 -C "$marpa_work" libmarpa_version.sh
cp "$marpa_work/libmarpa_version.sh" "$marpa_work/work/etc/libmarpa_version.sh"
make -j1 -C "$marpa_work/work/dev" api.texi
make -j1 -C "$marpa_work/work/dev" sources
make -j1 -C "$marpa_work/work/ami" sources
make -j1 -C "$marpa_work/work/public" all
```

The version-script copy and `api.texi` target are needed on a pristine source
tree. `api.texi` runs Emacs in batch mode. The remaining source targets use
`ctangle`/Perl and assemble `marpa.h` and `marpa_codes.c`; they do not build PDFs.
The AVL, TAVL and obstack C/header files are already present.
[Core source targets](https://github.com/jeffreykegler/libmarpa/blob/243540393a690efc2f6aca20f9d57c4099dcf646/work/dev/Makefile),
[AMI targets](https://github.com/jeffreykegler/libmarpa/blob/243540393a690efc2f6aca20f9d57c4099dcf646/work/ami/Makefile),
[public header generation](https://github.com/jeffreykegler/libmarpa/blob/243540393a690efc2f6aca20f9d57c4099dcf646/work/public/Makefile).

Collect the same six C translation units and their headers as the original
standalone builder. Generate the small C99 configuration from upstream's
template, retaining its copyright/license notice. The two standard integer
headers below are prerequisites on this Linux/WSL route.
[Upstream file map](https://github.com/jeffreykegler/libmarpa/blob/243540393a690efc2f6aca20f9d57c4099dcf646/work/ac/copier.list)
and [configuration template](https://github.com/jeffreykegler/libmarpa/blob/243540393a690efc2f6aca20f9d57c4099dcf646/cmake/config.h.cmake).

```sh
for relative in \
  dev/marpa.c ami/marpa_ami.c ami/marpa_ami.h \
  avl/marpa_avl.c avl/marpa_avl.h tavl/marpa_tavl.c tavl/marpa_tavl.h \
  obs/marpa_obs.c obs/marpa_obs.h \
  public/marpa.h public/marpa_codes.c public/marpa_codes.h
do
  cp "$marpa_work/work/$relative" "$marpa_standalone/"
done
cp "$marpa_work/LICENSE" "$marpa_work/work/shared/COPYING" \
  "$marpa_work/work/obs/COPYING.LESSER" "$marpa_standalone/"
cp "$marpa_work/work/avl/COPYING.LESSER" "$marpa_standalone/COPYING.LESSER.avl"
cp "$marpa_work/work/tavl/COPYING.LESSER" "$marpa_standalone/COPYING.LESSER.tavl"
sed \
  -e 's/@MARPA_LIB_VERSION@/11.0.13/g' \
  -e 's/@MARPA_LIB_MAJOR_VERSION@/11/g' \
  -e 's/@MARPA_LIB_MINOR_VERSION@/0/g' \
  -e 's/@MARPA_LIB_MICRO_VERSION@/13/g' \
  -e 's/@HAVE_STDINT_H@/1/g' -e 's/@HAVE_INTTYPES_H@/1/g' \
  "$marpa_work/cmake/config.h.cmake" > "$marpa_standalone/config.h"
for unit in marpa marpa_ami marpa_avl marpa_tavl marpa_obs marpa_codes
do
  cc -O2 -std=c99 -DHAVE_CONFIG_H=1 -DMARPA_DEBUG=0 \
    -I"$marpa_standalone" -c "$marpa_standalone/$unit.c" \
    -o "$marpa_standalone/$unit.o"
done
mkdir -p "$bench_prefix/include" "$bench_prefix/lib"
test ! -e "$bench_prefix/lib/libmarpa.a"
ar rcs "$bench_prefix/lib/libmarpa.a" "$marpa_standalone"/marpa*.o
cp "$marpa_standalone/marpa.h" "$bench_prefix/include/marpa.h"
nm -g --defined-only "$bench_prefix/lib/libmarpa.a" | \
  grep -E '_marpa_r_(is_use_leo_set|is_use_leo|earley_set_size)$'
grep -E '^#define MARPA_(MAJOR|MINOR|MICRO)_VERSION' "$bench_prefix/include/marpa.h"
```

The symbol check should show all three private interfaces and the header should
show version 11/0/13. Preserve the complete source checkout and its component
license notices: the auxiliary AVL/TAVL/obstack files have their own notices;
the top-level permission notice is not a substitute for them. No dependency
source or library is shipped in this benchmark package.

## Connect the local prefix to the benchmark

Run in the same Bash session, or recreate the variables from the first block.
The first-party wrapper sources belong to root `adapters/`; dependency headers
and libraries remain in this local prefix. `benchmarks/build.py` compiles only
the wrapper whose optional dependency is available, and hashes adapter sources
along with the current engine and benchmark sources.
The environment changes apply only to that shell and its children. Local
`pkg-config` metadata selects the pinned libraries; the loader path selects the
same shared libraries at execution time.

```sh
export PKG_CONFIG_PATH="$bench_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$bench_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
test "$(pkg-config --modversion libpcre2-8)" = 10.42
test "$(pkg-config --modversion libhs)" = 5.4.2
cd "$bench_crank"
CXX=g++ python3 benchmarks/build.py \
  --marpa-source "$marpa_source" --marpa-prefix "$bench_prefix"
python3 benchmarks/run.py --suite smoke --engines crank,marpa,pcre2_interp,pcre2_jit,hyperscan --check-only
```

Review `build/benchmarks/build.json` before a timing campaign. The compiler,
flags, versions and source/header/library hashes are recorded. A failed build,
version mismatch, unavailable JIT or unsupported grammar remains a separate
status; it is not evidence of rejection. The fixture smoke is a new correctness
run, not an attempt to regenerate archived performance numbers.
