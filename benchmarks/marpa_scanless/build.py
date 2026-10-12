#!/usr/bin/env python3
"""Build benchmark frontends using an existing repository libcrank.a (Linux)."""
import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SOURCES = {"configured": ("crank_public_runner.cpp", "crank_public_runner"),
           "batch": ("crank_public_runner_batched_drain.cpp", "crank_batched_drain"),
           "hardware-control": ("crank_public_runner_native_uncapped.cpp", "crank_public_runner_native_uncapped")}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def library_provenance(library, receipt_path):
    if not receipt_path.is_file():
        raise ValueError("a matching tools/build.py library receipt is required")
    receipt = json.loads(receipt_path.read_text())
    if receipt.get("target") != "libcrank" or receipt.get("binary_sha256") != digest(library):
        raise ValueError("library does not match its build receipt")
    dependencies = {}
    identities = []
    for obj in receipt.get("objects", []):
        identities.append(obj["identity"])
        dependencies.update(obj.get("dependencies", {}))
    if not dependencies:
        raise ValueError("library receipt has no source dependencies")
    for name, expected in dependencies.items():
        path = REPO / name
        if not path.is_file() or digest(path) != expected:
            raise ValueError("library source dependency changed: " + name)
    try:
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
        dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=REPO, text=True).strip())
        production_dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--", "engine", "build.json"], cwd=REPO, text=True).strip())
    except (OSError, subprocess.CalledProcessError):
        revision, dirty, production_dirty = None, None, None
    return {"library_build_receipt_sha256": digest(receipt_path),
            "library_build_identities": identities,
            "production_dependency_sha256": dict(sorted(dependencies.items())),
            "repository_revision": revision, "repository_dirty": dirty,
            "production_sources_dirty": production_dirty,
            "revision_is_not_a_substitute_for_content_hashes": True}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--library", type=Path, default=REPO / "build/hybrid/libcrank.a")
    ap.add_argument("--library-receipt", type=Path, help="defaults to the library directory's libcrank.build.json")
    ap.add_argument("--variant", choices=list(SOURCES), default="configured")
    ap.add_argument("--output", type=Path, default=HERE / "bin")
    args = ap.parse_args()
    library = args.library.resolve()
    if not library.is_file():
        ap.error("build the library first: python3 tools/build.py libcrank")
    try:
        provenance = library_provenance(library, args.library_receipt or library.parent / "libcrank.build.json")
    except ValueError as exc:
        ap.error(str(exc))
    if shutil.disk_usage(args.output.resolve().parent).free < 3 * 1024 ** 3:
        ap.error("less than 3 GiB free")
    args.output.mkdir(parents=True, exist_ok=True)
    source, name = SOURCES[args.variant]
    output = args.output.resolve() / name
    library_hash = digest(library)
    source_hash = digest(HERE / source)
    compiler_command = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=c++20", "-O2", "-g0", "-pthread", "-Wall", "-Wextra"]
    command = compiler_command + flags + ["-I" + str(REPO / "engine"), "-I" + str(REPO),
               str(HERE / source), str(library), "-o", str(output)]
    subprocess.run(command, check=True, timeout=90)
    after = library_provenance(library, args.library_receipt or library.parent / "libcrank.build.json")
    if (provenance["production_dependency_sha256"] != after["production_dependency_sha256"]
            or provenance["library_build_receipt_sha256"] != after["library_build_receipt_sha256"]
            or digest(library) != library_hash or digest(HERE / source) != source_hash):
        ap.error("library provenance changed during frontend build; rebuild")
    receipt = {"variant": args.variant, "source_sha256": source_hash,
               "library_sha256": library_hash,
               "binary_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
               "compiler": subprocess.check_output(compiler_command + ["--version"], text=True).splitlines()[0],
               "compiler_command": compiler_command, "compiler_flags": flags,
               "historical_measurement": False, **provenance}
    output.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(output)


if __name__ == "__main__":
    main()
