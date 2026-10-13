#!/usr/bin/env python3
"""Bounded allocation controls on a chosen repository tree; no installations."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import time

HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=HERE.parents[1])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--fault-controls", action="store_true")
    parser.add_argument("--skip-library-build", action="store_true")
    parser.add_argument("--budget-seconds", type=float, default=180)
    args = parser.parse_args()
    if not args.smoke and not args.fault_controls:
        parser.error("select --smoke and/or --fault-controls")
    if os.name != "posix" or not shutil.which("timeout"):
        parser.error("existing Linux compiler, ar, Python and GNU timeout are required")
    if not 0 < args.budget_seconds <= 600:
        parser.error("budget must be between zero and 600 seconds")
    repo = args.repo.resolve()
    output = args.output.resolve() if args.output else repo / "build" / ("allocation-check-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    if output.exists():
        parser.error("use a new output directory; saved evidence is never overwritten")
    if not repo.is_dir() or not (repo / "tools/build.py").is_file():
        parser.error("--repo must identify a Crank source checkout")
    if shutil.disk_usage(repo).free < 3 * 1024 ** 3 + 128 * 1024 ** 2:
        parser.error("less than 3 GiB free plus build reserve")
    output.mkdir(parents=True)
    started = time.monotonic()
    records = []
    identity = {"historical_measurement": False, "selected_combination_measured_by_saved_tables": False,
                "budget_seconds": args.budget_seconds, "commands": records,
                "source_sha256": {name: sha(HERE / name) for name in
                                  ["runner.cpp", "fault_controls.cpp", "corpus.json", "supplemental.json", "reproduce.py"]}}
    source_snapshot = output / "source"
    source_snapshot.mkdir()
    for name, expected in identity["source_sha256"].items():
        shutil.copyfile(HERE / name, source_snapshot / name)
        if sha(source_snapshot / name) != expected or sha(HERE / name) != expected:
            raise RuntimeError("companion source changed while freezing: " + name)
    identity["input_sha256"] = {}
    identity["executables"] = {}
    library = None
    library_hash = None
    dependencies = {}

    def verify_frozen_inputs():
        for name, expected in identity["source_sha256"].items():
            if sha(HERE / name) != expected or sha(source_snapshot / name) != expected:
                raise RuntimeError("companion/corpus bytes changed: " + name)
        if library_hash is not None:
            if sha(library) != library_hash or any(sha(repo / name) != expected for name, expected in dependencies.items()):
                raise RuntimeError("library/source bytes changed after admission")
        for name, expected in identity["input_sha256"].items():
            if sha(output / name) != expected:
                raise RuntimeError("materialized fixture bytes changed: " + name)

    def stop_group(child):
        # The session/process group belongs to this invocation, including GNU
        # timeout and its descendants. Kill the group even if its leader exits.
        previous = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait(timeout=5)
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)

    def interrupt(signum, frame):
        raise InterruptedError("reproduction interrupted")

    signal.signal(signal.SIGTERM, interrupt)

    def save():
        (output / "run.json").write_text(json.dumps(identity, indent=2) + "\n")

    def run(label, command, seconds):
        verify_frozen_inputs()
        binary_name = Path(str(command[0])).name
        if binary_name in identity["executables"] and sha(Path(command[0])) != identity["executables"][binary_name]:
            raise RuntimeError("executable bytes changed before launch: " + binary_name)
        if shutil.disk_usage(output).free < 3 * 1024 ** 3:
            raise RuntimeError("3 GiB free-space floor reached")
        allowance = min(seconds, args.budget_seconds - (time.monotonic() - started))
        if allowance < 1:
            raise RuntimeError("bounded reproduction budget exhausted")
        begin = time.monotonic()
        with (output / (label + ".stdout")).open("w") as stdout, (output / (label + ".stderr")).open("w") as stderr:
            child = subprocess.Popen(["timeout", "--kill-after=2", str(allowance)] + list(map(str, command)),
                                     stdout=stdout, stderr=stderr, cwd=repo, start_new_session=True)
            try:
                child.wait(timeout=allowance + 5)
            except BaseException as exc:
                try:
                    stop_group(child)
                finally:
                    records.append({"label": label, "status": type(exc).__name__,
                                    "returncode": child.returncode,
                                    "child_wall_seconds": time.monotonic() - begin})
                    save()
                raise
            if child.returncode:
                stop_group(child)
        record = {"label": label, "returncode": child.returncode,
                  "child_wall_seconds": time.monotonic() - begin}
        records.append(record)
        save()
        if child.returncode:
            raise RuntimeError(label + " failed; retain its receipt/logs, not a Boolean rejection")
        verify_frozen_inputs()
        return (output / (label + ".stdout")).read_text()

    save()
    if not args.skip_library_build:
        run("library-build", [os.sys.executable, repo / "tools/build.py", "libcrank"], args.budget_seconds)
    library = repo / "build/hybrid/libcrank.a"
    receipt_path = repo / "build/hybrid/libcrank.build.json"
    if not library.is_file() or not receipt_path.is_file():
        raise RuntimeError("matching existing libcrank.a and build receipt required")
    receipt = json.loads(receipt_path.read_text())
    library_hash = sha(library)
    if receipt.get("target") != "libcrank" or receipt.get("binary_sha256") != library_hash:
        raise RuntimeError("library hash differs from its source-build receipt")
    for obj in receipt.get("objects", []):
        for name, expected in obj.get("dependencies", {}).items():
            if name in dependencies and dependencies[name] != expected:
                raise RuntimeError("conflicting shared-header hashes in library receipt: " + name)
            if sha(repo / name) != expected:
                raise RuntimeError("library object source dependency changed: " + name)
            dependencies[name] = expected
    if not dependencies:
        raise RuntimeError("library build receipt has no source dependencies")
    for name, expected in dependencies.items():
        if sha(repo / name) != expected:
            raise RuntimeError("library source dependency changed: " + name)
    shutil.copyfile(receipt_path, output / "library-build-receipt.json")
    identity.update(library_sha256=library_hash, library_build_receipt_sha256=sha(receipt_path),
                    production_dependency_sha256=dependencies)
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=c++20", "-O2", "-pthread", "-I" + str(repo / "engine"), "-I" + str(repo)]
    identity["compiler_command"] = compiler
    identity["compiler_version"] = run("compiler-version", compiler + ["--version"], 5).splitlines()[0]
    for name, source, probe in [("timing", "runner.cpp", False), ("probe", "runner.cpp", True),
                                ("fault_controls", "fault_controls.cpp", True)]:
        if name == "fault_controls" and not args.fault_controls:
            continue
        if name in ("timing", "probe") and not args.smoke:
            continue
        command = compiler + flags + (["-DCRANK_ALLOC_PROBE=1"] if probe else []) + [source_snapshot / source, library, "-o", output / name]
        run(name + "-build", command, 90)
        identity["executables"][name] = sha(output / name)
    if sha(library) != library_hash or any(sha(repo / name) != expected for name, expected in dependencies.items()):
        raise RuntimeError("library/source bytes changed during frontend linking")
    save()
    if args.fault_controls:
        lines = run("fault-controls", [output / "fault_controls"], 60).splitlines()
        summary = [json.loads(line) for line in lines]
        if not summary or summary[-1].get("status") != "passed":
            raise RuntimeError("fault-control pass receipt missing")
        identity["fault_controls"] = summary
        save()
    if args.smoke:
        cases = {}
        for name in ["corpus.json", "supplemental.json"]:
            verify_frozen_inputs()
            cases.update({c["id"]: c for c in json.loads((source_snapshot / name).read_text())["cases"]})
        identity["smoke_receipts"] = []
        for case_id, phase in [("nested_capture_aliases_256", "equivalence"),
                               ("dfa_distinct_byte_chain_256", "regular")]:
            case = cases[case_id]
            verify_frozen_inputs()
            pattern, subject = output / (case_id + ".pattern"), output / (case_id + ".input")
            pattern.write_bytes(case["pattern"].encode("latin-1"))
            subject.write_bytes(case["subject"].encode("latin-1"))
            identity["input_sha256"][pattern.name] = hashlib.sha256(case["pattern"].encode("latin-1")).hexdigest()
            identity["input_sha256"][subject.name] = hashlib.sha256(case["subject"].encode("latin-1")).hexdigest()
            verify_frozen_inputs()
            for mode in ["probe", "timing"]:
                label = case_id + "-" + mode
                result = json.loads(run(label, [output / mode, pattern, subject, phase, "1", mode], 8))
                # These selected constructor controls return structural counts, not Booleans.
                # Both have more than two source/DFA states; 2 is the runner's ResourceLimit marker.
                if result.get("failures") or result.get("reps") != 1 or result.get("phase") != phase or result.get("verdict", 0) <= 2:
                    raise RuntimeError("constructor smoke failed or exhausted a resource allowance")
                identity["smoke_receipts"].append({"case": case_id, "mode": mode,
                                                  "pattern_sha256": sha(pattern), "subject_sha256": sha(subject),
                                                  "receipt": result})
                save()
    identity.update(status="passed", elapsed_seconds=time.monotonic() - started)
    save()
    print(json.dumps({"status": "passed", "historical_measurement": False,
                      "elapsed_seconds": identity["elapsed_seconds"]}))


if __name__ == "__main__":
    main()
