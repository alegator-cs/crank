#!/usr/bin/env python3
"""Build and run the isolated predictive-view correctness probes sequentially."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROBES = (
    "transfer_scope", "runtime_probe", "slot_authority_probe",
    "slot_execution_probe", "payload_view_probe",
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def input_hashes(source, library, target):
    paths = [source, *HERE.glob("*.hpp"), *(ROOT / "engine").glob("*.hpp"),
             *(ROOT / "tests/helpers").glob("*.hpp")]
    hashes = {str(path.relative_to(ROOT)): digest(path) for path in paths}
    if target != "transfer_scope":
        hashes["linked_library"] = digest(library)
    return hashes


def worker_limits():
    # Optional POSIX bounds; Windows keeps the subprocess timeout below.
    import resource
    if hasattr(resource, "RLIMIT_AS"):
        resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
    if hasattr(os, "sched_getaffinity"):
        os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
    os.nice(10)


def run(command, stdout, stderr, timeout):
    options = {"start_new_session": True, "preexec_fn": worker_limits} if os.name == "posix" else {}
    with stdout.open("w") as out, stderr.open("w") as err:
        process = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err, **options)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.wait()
            code = None
    print(stdout.read_text(errors="replace")[-4000:], end="", flush=True)
    print(stderr.read_text(errors="replace")[-4000:], end="", flush=True)
    return {
        "command": command, "exit_code": code,
        "status": "passed" if code == 0 else "timeout" if code is None else "failed",
        "timeout_seconds": timeout,
        "stdout_sha256": digest(stdout), "stderr_sha256": digest(stderr),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("targets", nargs="*", help="Probe names; default is all five")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--library", type=Path, default=ROOT / "build/hybrid/libcrank.a")
    parser.add_argument("--output", type=Path, default=ROOT / "build/predictive-views")
    parser.add_argument("--build-timeout", type=int, default=180)
    parser.add_argument("--check-timeout", type=int, default=45)
    args = parser.parse_args()
    if args.list:
        print("\n".join(PROBES))
        return 0
    targets = args.targets or list(PROBES)
    if any(target not in PROBES for target in targets):
        parser.error("Unknown target; use --list")
    if not args.build and not args.check:
        parser.error("Choose --build, --check, or both")
    if args.build_timeout <= 0 or args.check_timeout <= 0:
        parser.error("Timeouts must be positive")
    library = args.library.resolve()
    if args.build and any(target != "transfer_scope" for target in targets) and not library.is_file():
        parser.error("Build the current library first: python3 tools/build.py libcrank")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=c++20", "-O2", "-pthread", "-Wall", "-Wextra", "-Wpedantic",
             "-I" + str(HERE), "-I" + str(ROOT / "engine"), "-I" + str(ROOT)]
    flags += shlex.split(os.environ.get("EXTRA_CXXFLAGS", ""))
    records = []
    for target in targets:
        source = HERE / (target + ".cpp")
        binary = output / (target + (".exe" if os.name == "nt" else ""))
        build_receipt = output / (target + ".build.json")
        if args.build:
            inputs = input_hashes(source, library, target)
            command = cxx + flags + [str(source)]
            if target != "transfer_scope":
                command += [str(library)]
            command += ["-o", str(binary)]
            print("build", target, flush=True)
            record = run(command, output / (target + ".build.stdout.txt"),
                         output / (target + ".build.stderr.txt"), args.build_timeout)
            record.update(target=target, stage="build", source_sha256=digest(source))
            records.append(record)
            if record["status"] != "passed":
                break
            if inputs != input_hashes(source, library, target):
                raise RuntimeError("Build input changed; rebuild: " + target)
            build_receipt.write_text(json.dumps({"inputs": inputs,
                "binary_sha256": digest(binary), "command": command}, indent=2) + "\n")
        if args.check:
            if not binary.is_file():
                parser.error("Missing probe executable; use --build first: " + str(binary))
            if not build_receipt.is_file():
                parser.error("Missing build receipt; use --build first: " + target)
            built = json.loads(build_receipt.read_text())
            if built["inputs"] != input_hashes(source, library, target) or built["binary_sha256"] != digest(binary):
                parser.error("Probe inputs or executable changed; rebuild: " + target)
            command = [str(binary)]
            if target == "runtime_probe":
                command += ["--check-only"]
            elif target in ("transfer_scope", "slot_authority_probe"):
                command += ["--out", str(output / (target + ".csv"))]
            print("check", target, flush=True)
            record = run(command, output / (target + ".check.stdout.txt"),
                         output / (target + ".check.stderr.txt"), args.check_timeout)
            record.update(target=target, stage="check", binary_sha256=digest(binary))
            records.append(record)
            if record["status"] != "passed":
                break
    passed = len(records) == len(targets) * (int(args.build) + int(args.check)) and all(
        record["status"] == "passed" for record in records)
    receipt = {"status": "passed" if passed else "incomplete_or_failed",
               "performance_measurement": False, "runs": records}
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
