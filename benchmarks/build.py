#!/usr/bin/env python3
"""Build optional adapters from already supplied dependencies; never fetch/install."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=HERE.parent)
    parser.add_argument("--marpa-source", type=Path, help="existing checkout at the pinned commit")
    parser.add_argument("--marpa-prefix", type=Path, help="existing generated header/library prefix")
    parser.add_argument("--build-timeout", type=int, default=300)
    args = parser.parse_args()
    if args.build_timeout <= 0:
        parser.error("build timeout must be positive")
    root = args.project.resolve()
    out = root / "build" / "benchmarks"
    out.mkdir(parents=True, exist_ok=True)
    pins = json.loads((HERE / "DEPENDENCIES.json").read_text())
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler or not shutil.which(compiler[0]):
        parser.error("CXX compiler is unavailable")
    flags = ["-std=c++20", "-O2", "-pthread", "-I" + str(root / "engine"), "-I" + str(root), "-I" + str(HERE)]
    flags += shlex.split(os.environ.get("EXTRA_CXXFLAGS", ""))
    receipt = dict(schema_version=1, compiler=compiler,
                   compiler_version=subprocess.check_output(compiler + ["--version"], text=True).splitlines()[0],
                   flags=flags, engines={}, source_hashes={})

    def command(cmd, name):
        log = out / (name + ".build.log")
        try:
            with log.open("w") as stream:
                process = subprocess.Popen(cmd, cwd=root, stdout=stream, stderr=subprocess.STDOUT,
                                           start_new_session=os.name == "posix")
                try:
                    process.wait(timeout=args.build_timeout)
                except subprocess.TimeoutExpired:
                    if os.name == "posix": os.killpg(process.pid, signal.SIGKILL)
                    else: process.kill()
                    process.wait()
                    raise
            return process.returncode == 0, "see " + log.name
        except subprocess.TimeoutExpired:
            return False, "build timeout; see " + log.name

    ok, detail = command([sys.executable, str(root / "tools" / "build.py"), "libcrank"], "libcrank")
    if not ok:
        raise SystemExit("Crank library build failed: " + detail)
    library = root / "build" / "hybrid" / "libcrank.a"
    receipt["libcrank_sha256"] = digest(library)
    library_receipt = library.with_suffix(".build.json")
    if library_receipt.exists():
        receipt["libcrank_build_receipt"] = json.loads(library_receipt.read_text())
    manifest = json.loads((root / "build.json").read_text())
    source_paths = set(manifest["sources"])
    source_paths.update(p.relative_to(root).as_posix() for p in (root / "engine").glob("*.hpp"))
    for name in sorted(source_paths):
        receipt["source_hashes"][name] = digest(root / name)
    for path in sorted(HERE.rglob("*")):
        if path.is_file() and path.suffix in (".cpp", ".hpp", ".py", ".json"):
            receipt["source_hashes"]["benchmarks/" + path.relative_to(HERE).as_posix()] = digest(path)
    adapters = root / "adapters"
    if not (adapters / "adapter_error.hpp").is_file():
        parser.error("root adapters/ comparison wrappers are missing")
    for path in sorted(adapters.rglob("*")):
        if path.is_file() and path.suffix in (".cpp", ".hpp"):
            receipt["source_hashes"][path.relative_to(root).as_posix()] = digest(path)

    def unavailable(modes, why):
        for mode in modes:
            receipt["engines"][mode] = dict(available=False, status="unavailable", detail=why)

    def build(name, modes, defines=(), extra_sources=(), compile_flags=(), link_flags=(), metadata=None):
        binary = out / ("runner-" + name)
        cmd = compiler + flags + list(defines) + list(compile_flags)
        cmd += [str(HERE / "runner.cpp")] + [str(p) for p in extra_sources]
        cmd += [str(library)] + list(link_flags) + ["-o", str(binary)]
        success, why = command(cmd, name)
        if not success:
            unavailable(modes, "adapter build failed: " + why)
            return
        for mode in modes:
            receipt["engines"][mode] = dict(available=True, binary=binary.name,
                                            binary_sha256=digest(binary), **(metadata or {}))

    build("crank", ["crank", "native_ast", "assisted2", "assisted4", "assisted_always4"])
    if not receipt["engines"]["crank"]["available"]:
        raise SystemExit("Crank benchmark runner did not build")

    marpa_modes = ["marpa"]
    if not args.marpa_source or not args.marpa_prefix:
        unavailable(marpa_modes, "supply both --marpa-source and --marpa-prefix; no vendor is bundled")
    else:
        try:
            source = args.marpa_source.resolve()
            commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                             text=True, stderr=subprocess.DEVNULL).strip()
            if commit != pins["libmarpa"]["source_commit"]:
                raise ValueError("Libmarpa source commit differs from pin")
            dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"], text=True)
            if dirty:
                raise ValueError("Libmarpa pinned checkout has tracked changes")
            prefix = args.marpa_prefix.resolve()
            header = next((p for p in (prefix / "include" / "marpa.h", prefix / "marpa.h") if p.is_file()), None)
            archive = next((p for p in (prefix / "lib" / "libmarpa.a", prefix / "libmarpa.a", prefix / ".libs" / "libmarpa.a") if p.is_file()), None)
            if header is None or archive is None:
                raise ValueError("prefix needs generated marpa.h and static libmarpa.a")
            build("marpa", marpa_modes, ["-DBENCH_MARPA=1"], [adapters / "marpa_adapter.cpp"],
                  ["-I" + str(header.parent)], [str(archive)],
                  dict(version=pins["libmarpa"]["version"], source_commit=commit,
                       header_sha256=digest(header), library_sha256=digest(archive), leo=True))
        except (OSError, subprocess.CalledProcessError, ValueError) as error:
            unavailable(marpa_modes, str(error))

    for name, modes, define in (("pcre2", ["pcre2_interp", "pcre2_jit"], "BENCH_PCRE2"),
                                ("hyperscan", ["hyperscan"], "BENCH_HYPERSCAN")):
        package = pins[name]["pkg_config"]
        if not shutil.which("pkg-config"):
            unavailable(modes, "pkg-config unavailable")
            continue
        try:
            version = subprocess.check_output(["pkg-config", "--modversion", package], text=True,
                                             stderr=subprocess.DEVNULL).strip()
            if version != pins[name]["version"]:
                unavailable(modes, "installed " + version + " differs from pinned " + pins[name]["version"])
                continue
            cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", package], text=True))
            if name == "hyperscan":
                # libhs.pc exports include/hs for <hs.h>; this adapter uses
                # <hs/hs.h>, which also needs the local prefix's include root.
                includedir = subprocess.check_output(["pkg-config", "--variable=includedir", package], text=True).strip()
                if not includedir:
                    raise ValueError("libhs pkg-config metadata lacks includedir")
                cflags.append("-I" + includedir)
            libraries = shlex.split(subprocess.check_output(["pkg-config", "--libs", package], text=True))
            build(name, modes, ["-D" + define + "=1"], [adapters / (name + "_adapter.cpp")], compile_flags=cflags,
                  link_flags=libraries, metadata=dict(version=version, pkg_config=package))
        except (OSError, subprocess.CalledProcessError, ValueError) as error:
            unavailable(modes, "pinned dependency unavailable: " + package)
    (out / "build.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({k: v for k, v in receipt.items() if k != "source_hashes" and k != "libcrank_build_receipt"}, indent=2))


if __name__ == "__main__":
    main()
