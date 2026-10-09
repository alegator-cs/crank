#!/usr/bin/env python3
"""Correctness-gated full-span byte benchmarks with bounded child processes."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import signal
import subprocess
import sys
import time
from fixtures import cases

HERE = Path(__file__).resolve().parent
CORE_ENGINES = ("crank", "marpa", "pcre2_interp", "pcre2_jit", "hyperscan")
ASSISTANCE_ENGINES = ("native_ast", "assisted2", "assisted4", "assisted_always4")
ENGINES = CORE_ENGINES + ASSISTANCE_ENGINES
NONDECISIONS = {"unsupported", "resource_limit", "error", "incorrect", "unavailable"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_protocol(text, returncode, expected, operation, iterations, samples):
    try:
        rows = [json.loads(line) for line in text.splitlines() if line.strip()]
        if not rows or len(rows) > 2 or any(not isinstance(row, dict) for row in rows):
            raise ValueError("expected one failure result or prepared/result pair")
        result = rows[-1]
        if result.get("stage") != "result":
            raise ValueError("missing final result")
        status = result.get("status")
        if status == "complete":
            if len(rows) != 2 or rows[0].get("stage") != "prepared" or returncode != 0:
                raise ValueError("complete result needs prepared stage and zero exit")
            if type(result.get("matched")) is not bool or result["matched"] != expected:
                return dict(status="incorrect", matched=None, detail="verdict differs from fixture")
            for key in ("prepare_ns", "first_query_ns"):
                value = result.get(key)
                if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
                    raise ValueError("invalid duration: " + key)
            target = samples if operation == "measure" else 0
            actual_iterations = result.get("iterations")
            if type(actual_iterations) is not int or (target and not 1 <= actual_iterations <= 1000000):
                raise ValueError("invalid iteration count")
            if (iterations or not target) and actual_iterations != (iterations if target else 0):
                raise ValueError("iteration count mismatch")
            for key in ("warm_wall_ns_per_query", "warm_cpu_ns_per_query", "warm_batch_ns"):
                values = result.get(key)
                if not isinstance(values, list) or len(values) != target:
                    raise ValueError("sample count mismatch: " + key)
                nullable = key == "warm_cpu_ns_per_query"
                if any(not (nullable and v is None) and
                       (type(v) not in (int, float) or not math.isfinite(v) or v < 0) for v in values):
                    raise ValueError("invalid warm samples")
            if target and any(not math.isclose(batch / actual_iterations, wall, rel_tol=1e-10, abs_tol=1e-6)
                              for batch, wall in zip(result["warm_batch_ns"], result["warm_wall_ns_per_query"])):
                raise ValueError("batch/query duration mismatch")
            result["version"] = rows[0].get("version")
            return result
        if status not in NONDECISIONS or "matched" not in result or result["matched"] is not None:
            raise ValueError("unknown status or verdict attached to a nondecision")
        if len(rows) == 2 and rows[0].get("stage") != "prepared":
            raise ValueError("out-of-order stages")
        if status in {"unsupported", "resource_limit", "unavailable"} and returncode != 0:
            raise ValueError("nondecision returned unexpected process exit")
        return result
    except (ValueError, TypeError, json.JSONDecodeError) as error:
        return dict(status="protocol_error", matched=None, detail=str(error))


def launch(command, timeout, memory_mib, cpus):
    def limits():
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (memory_mib * 1024**2,) * 2)
        if cpus is not None and hasattr(os, "sched_setaffinity"):
            os.sched_setaffinity(0, set(cpus))

    start = time.monotonic()
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   start_new_session=os.name == "posix",
                                   preexec_fn=limits if os.name == "posix" else None)
    except (OSError, subprocess.SubprocessError) as error:
        return dict(stdout="", stderr=str(error), returncode=None, process_wall_seconds=time.monotonic() - start,
                    process_status="error")
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        status = "crash" if process.returncode < 0 else None
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
        stdout, stderr = process.communicate()
        status = "timeout"
    return dict(stdout=stdout, stderr=stderr[-4000:], returncode=process.returncode,
                process_wall_seconds=time.monotonic() - start, process_status=status)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=HERE.parent)
    parser.add_argument("--suite", choices=("smoke", "cfg", "practical", "rejection", "assistance", "all"), default="all")
    parser.add_argument("--engines", help="default practical: Crank/Marpa; assistance: native/helper controls; others: all external engines")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--iterations", type=int, default=0, help="fixed batch size; default calibrates each fresh process")
    parser.add_argument("--target-ms", type=float, default=15, help="bounded calibration target per batch")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--memory-mib", type=int, default=2048)
    parser.add_argument("--max-states", type=int, default=2000000)
    parser.add_argument("--max-steps", type=int, default=8000000)
    parser.add_argument("--helper-steps", type=int, default=1000000)
    parser.add_argument("--min-native-steps", type=int, default=512)
    parser.add_argument("--min-input-bytes", type=int, default=1024)
    affinity = parser.add_mutually_exclusive_group()
    affinity.add_argument("--cpu", type=int, help="one already available logical CPU; default first allowed CPU")
    affinity.add_argument("--cpus", help="comma-separated allowed logical CPUs for explicit helper-thread comparisons")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--plot", action="store_true")
    args = parser.parse_args()
    if not (0 <= args.iterations <= 1000000 and 0 < args.samples <= 31 and 0 < args.repeats <= 100):
        parser.error("iterations/samples/repeats are outside supported positive bounds")
    if not math.isfinite(args.timeout) or args.timeout <= 0 or args.memory_mib <= 0:
        parser.error("timeout and memory must be finite positive limits")
    if not math.isfinite(args.target_ms) or not 0.000001 <= args.target_ms <= 1000:
        parser.error("target batch milliseconds must be finite and within (0, 1000]")
    if not (0 < args.max_states <= 1000000000 and 0 < args.max_steps <= 1000000000):
        parser.error("native budgets are outside supported positive bounds")
    if not (0 < args.helper_steps <= 1000000000 and 0 < args.min_native_steps <= 1000000000 and 0 < args.min_input_bytes <= 67108864):
        parser.error("assistance budgets/gates are outside supported positive bounds")
    defaults = ("crank", "marpa") if args.suite == "practical" else ASSISTANCE_ENGINES if args.suite == "assistance" else CORE_ENGINES
    selected = args.engines.split(",") if args.engines else list(defaults)
    if not selected or len(set(selected)) != len(selected) or any(e not in ENGINES for e in selected):
        parser.error("select unique known engine names separated by commas")
    root = args.project.resolve()
    build = root / "build" / "benchmarks"
    config = json.loads((build / "build.json").read_text())
    for name, expected_hash in config["source_hashes"].items():
        if not (root / name).is_file() or digest(root / name) != expected_hash:
            parser.error("source changed since benchmark build: " + name)
    if digest(root / "build" / "hybrid" / "libcrank.a") != config["libcrank_sha256"]:
        parser.error("Crank library changed since benchmark build")
    allowed = os.sched_getaffinity(0) if hasattr(os, "sched_getaffinity") else None
    try:
        cpus = [int(value) for value in args.cpus.split(",")] if args.cpus else ([args.cpu] if args.cpu is not None else [min(allowed)] if allowed else None)
    except ValueError:
        parser.error("CPU list must contain integer IDs")
    if cpus is not None and (len(set(cpus)) != len(cpus) or any(c < 0 or allowed is not None and c not in allowed for c in cpus)):
        parser.error("select unique CPUs in the current allowed set")
    for engine in selected:
        meta = config["engines"].get(engine, {})
        if meta.get("available"):
            binary = build / meta["binary"]
            if not binary.is_file() or digest(binary) != meta["binary_sha256"]:
                parser.error("benchmark executable changed: " + engine)
    started = datetime.now(timezone.utc)
    folder = build / ("run-" + started.strftime("%Y%m%dT%H%M%S.%fZ"))
    folder.mkdir()
    fixture_rows = cases(args.suite)
    inputs = []
    for fixture in fixture_rows:
        if not fixture["id"].replace("_", "").isalnum():
            parser.error("unsafe fixture ID")
        pattern = folder / (fixture["id"] + ".pattern")
        subject = folder / (fixture["id"] + ".input")
        pattern.write_bytes(fixture["pattern"]); subject.write_bytes(fixture["subject"])
        inputs.append(dict(id=fixture["id"], group=fixture["group"], expected=fixture["expected"],
                           bytes=len(fixture["subject"]), oracle=fixture["oracle"],
                           pattern_sha256=digest(pattern), subject_sha256=digest(subject)))
    report = dict(schema_version=1, created_utc=started.isoformat(), scope="current API; full-span byte Boolean membership",
                  suite=args.suite, check_only=args.check_only, build=config,
                  machine=dict(system=platform.system(), release=platform.release(), machine=platform.machine(),
                               python=platform.python_version(), logical_cpus=os.cpu_count()),
                  limits=dict(timeout_seconds=args.timeout, address_space_mib=args.memory_mib,
                              address_space_enforced=os.name == "posix", cpus=cpus,
                              max_states=args.max_states, max_steps=args.max_steps),
                  assistance=dict(helper_steps=args.helper_steps, min_native_steps=args.min_native_steps,
                                  min_input_bytes=args.min_input_bytes, workers_include_foreground=True,
                                  symmetric_control_disabled_routes=["compiled_exact_programs", "bounded_word_prediction", "deterministic_programs",
                                  "normalized_exact_programs", "regional_execution", "regional_constraints", "exact_end_spines", "predictive_recursion", "predictive_exact_rims"]),
                  measurement=dict(iterations=args.iterations, target_batch_ms=args.target_ms,
                                   calibration_max_iterations=1000000, samples=args.samples, repeats=args.repeats,
                                   ordering="seeded initial engine shuffle, rotated by fresh-process repeat"),
                  fixtures=inputs, checks=[], measurements=[])
    output = args.output.resolve() if args.output else folder / "results.json"
    if output.exists():
        parser.error("refusing to overwrite an existing results file")
    output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        output.write_text(json.dumps(report, indent=2) + "\n")

    def query(fixture, engine, operation):
        meta = config["engines"].get(engine, {})
        row = dict(fixture=fixture["id"], engine=engine, expected=fixture["expected"])
        if not meta.get("available"):
            return dict(row, status="unavailable", matched=None, detail=meta.get("detail", "not built"))
        command = [str(build / meta["binary"]), engine, str(folder / (fixture["id"] + ".pattern")),
                   str(folder / (fixture["id"] + ".input")), str(int(fixture["expected"])), operation,
                   str(args.iterations), str(args.samples), str(args.max_states), str(args.max_steps), "v2",
                   str(round(args.target_ms * 1000000)), str(args.helper_steps), str(args.min_native_steps), str(args.min_input_bytes)]
        process = launch(command, args.timeout, args.memory_mib, cpus)
        if process["process_status"]:
            result = dict(status=process["process_status"], matched=None)
        else:
            result = parse_protocol(process["stdout"], process["returncode"], fixture["expected"],
                                    operation, args.iterations, args.samples)
        return dict(row, **result, returncode=process["returncode"], stderr=process["stderr"],
                    process_wall_seconds=process["process_wall_seconds"])

    gates = {}
    for fixture in fixture_rows:
        for engine in selected:
            row = query(fixture, engine, "check")
            report["checks"].append(row); gates[(fixture["id"], engine)] = row["status"]
            save()
    fatal = {"incorrect", "error", "crash", "protocol_error"}
    if any(row["status"] in fatal for row in report["checks"]):
        report["gate"] = "failed; no timing launched"; save()
        raise SystemExit("Correctness/protocol failure; see " + str(output))
    report["gate"] = ("passed for completed checks; nondecisions excluded from timing"
                      if any(row["status"] == "complete" for row in report["checks"]) else "no completed checks")
    if not args.check_only:
        order = selected.copy(); random.Random(20261009).shuffle(order)
        for repeat in range(args.repeats):
            balanced = order[repeat % len(order):] + order[:repeat % len(order)]
            for fixture in fixture_rows:
                for engine in balanced:
                    prior = gates[(fixture["id"], engine)]
                    row = query(fixture, engine, "measure") if prior == "complete" else dict(
                        fixture=fixture["id"], engine=engine, status=prior, matched=None,
                        detail="not timed because preflight was a nondecision")
                    row["repeat"] = repeat; report["measurements"].append(row); save()
                    if row["status"] in fatal:
                        report["gate"] = "measurement verdict/protocol failure; partial campaign invalid"; save()
                        raise SystemExit("Measurement failed; see " + str(output))
    report["finished_utc"] = datetime.now(timezone.utc).isoformat(); save()
    if args.plot and not args.check_only:
        subprocess.run([sys.executable, str(HERE / "plot.py"), str(output)], check=True)
    print(output)


if __name__ == "__main__":
    main()
