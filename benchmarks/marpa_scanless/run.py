#!/usr/bin/env python3
"""Run a finite sequential Linux campaign on new public synthetic fixtures."""
import argparse
import collections
import datetime
import hashlib
import json
import os
import resource
import shutil
import signal
import statistics
import subprocess
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCES = {"configured": "crank_public_runner.cpp", "batch": "crank_public_runner_batched_drain.cpp",
           "hardware-control": "crank_public_runner_native_uncapped.cpp"}
FAILURES = {"resource", "memory_limit", "timeout", "crash", "cancelled", "unsupported",
            "unavailable", "error", "protocol_error"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def classify(code, receipt, timed_out=False, cancelled=False):
    if cancelled:
        return "cancelled"
    if timed_out:
        return "timeout"
    if code < 0:
        return "crash"
    status = receipt.get("status", "protocol_error")
    if status in FAILURES:
        return status
    return "ok" if code == 0 and status == "ok" else "protocol_error"


def validate(receipt, engine, expected, records, count, samples):
    batches = receipt.get("warm_batches", [])
    if len(batches) != samples or any(b.get("count") != count for b in batches):
        raise ValueError("warm workload counts differ")
    if engine == "marpa":
        label = "accepted" if expected else "rejected"
        first = receipt.get("first", {})
        if not first.get("correct") or first.get("accepted") != expected:
            raise ValueError("incorrect first Boolean")
        if receipt.get("runtime_validation") != "passed_measured_rows":
            raise ValueError("Marpa runtime validation failed")
        if any(b.get("failures") or b.get("outcome_counts") != {label: count} for b in batches):
            raise ValueError("incorrect warm Boolean")
    elif receipt.get("verdicts") != [int(expected)] * records or receipt.get("queried_records") != records:
        raise ValueError("incorrect Crank Boolean")
    if engine == "progress4" and any(b.get("pool_after_drain", {}).get("active_helpers") != 0 for b in batches):
        raise ValueError("helper drain unverified")


def costs(receipts, count):
    result = {}
    for clock in ("wall", "cpu"):
        result["warm_" + clock + "_us"] = statistics.median(
            statistics.median(b[clock + "_ns"] / count / 1000 for b in r["warm_batches"]) for r in receipts)
        result["cold_public_" + clock + "_ms"] = statistics.median(
            (r["phases"]["cold_prepare_and_first_complete"][clock + "_ns"]
             - r["phases"].get("module_load", {}).get(clock + "_ns", 0)) / 1e6 for r in receipts)
        result["cold_import_inclusive_" + clock + "_ms"] = statistics.median(
            r["phases"]["cold_prepare_and_first_complete"][clock + "_ns"] / 1e6 for r in receipts)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("manifest", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--cpus", required=True, help="one or four available CPU IDs, comma-separated")
    ap.add_argument("--budget-seconds", type=float, default=60)
    ap.add_argument("--entry-seconds", type=float, default=3)
    ap.add_argument("--engines", default="auto,progress4,marpa")
    ap.add_argument("--families", help="optional pre-run comma-separated selection")
    ap.add_argument("--sizes", help="optional pre-run comma-separated selection")
    ap.add_argument("--variant", choices=["configured", "batch", "hardware-control"], default="configured")
    ap.add_argument("--quiet-marpa", action="store_true", help="separate warning-threshold diagnostic")
    args = ap.parse_args()
    cpus = [int(x) for x in args.cpus.split(",")]
    engines = args.engines.split(",")
    if not engines or len(set(engines)) != len(engines) or set(engines) - {"auto", "progress4", "marpa"}:
        ap.error("engines must be a unique subset of auto,progress4,marpa")
    if len(cpus) not in (1, 4) or len(set(cpus)) != len(cpus) or not set(cpus) <= os.sched_getaffinity(0):
        ap.error("provide one or four distinct CPU IDs available to this process")
    if "progress4" in engines and len(cpus) != 4:
        ap.error("Progress4 needs four distinct permitted CPUs")
    if not 0 < args.entry_seconds <= 180 or not 0 < args.budget_seconds <= 7200:
        ap.error("entry allowance must be 0..180s and campaign budget 0..7200s")
    if args.output.exists():
        ap.error("use a new campaign output directory")
    mpath = args.manifest.resolve()
    m = json.loads(mpath.read_text())
    if m.get("schema") != "public-synthetic-corpus-v1" or m.get("historical_measured_inputs") is not False:
        ap.error("this controller accepts the new public synthetic manifest only")
    count, samples, repeats = m["warm_count"], m["warm_samples"], m["repeats"]
    if count == 128 and args.variant != "batch":
        ap.error("COUNT=128 requires the per-input helper-drain batch runner")
    binary_name = {"configured": "crank_public_runner", "batch": "crank_batched_drain",
                   "hardware-control": "crank_public_runner_native_uncapped"}[args.variant]
    binary = HERE / "bin" / binary_name
    if set(engines) - {"marpa"} and not binary.is_file():
        ap.error("build the selected runner first with build.py")
    build_receipt = None
    build_path = binary.with_suffix(".json")
    if set(engines) - {"marpa"}:
        if not build_path.is_file():
            ap.error("selected runner needs its build.py .json receipt")
        build_receipt = json.loads(build_path.read_text())
        if (build_receipt.get("variant") != args.variant
                or build_receipt.get("binary_sha256") != digest(binary)
                or build_receipt.get("source_sha256") != digest(HERE / SOURCES[args.variant])
                or not build_receipt.get("library_sha256")
                or not build_receipt.get("production_dependency_sha256")):
            ap.error("selected binary/source/library provenance differs from its build receipt")
    marpa = HERE / ("marpa_scanless_runner_quiet.pl" if args.quiet_marpa else "marpa_scanless_runner.pl")
    marpa_hash = digest(marpa)
    entries = [r for r in m["entries"]
               if (not args.families or r["family"] in args.families.split(","))
               and (not args.sizes or r["size"] in args.sizes.split(","))]
    if not entries:
        ap.error("empty pre-run selection")
    for entry in entries:
        for binding in list(entry["grammars"].values()) + entry["corpora"]:
            if digest(mpath.parent / binding["path"]) != binding["sha256"]:
                ap.error("frozen input/grammar hash mismatch")
    if shutil.disk_usage(args.output.resolve().parent).free < 3 * 1024 ** 3:
        ap.error("less than 3 GiB free")
    args.output.mkdir()
    shutil.copyfile(mpath, args.output / "manifest.json")
    if build_receipt is not None:
        shutil.copyfile(build_path, args.output / "crank-build-receipt.json")
    started = time.monotonic()
    deadline = started + args.budget_seconds
    stopped, current = False, None

    def cancel(sig, frame):
        nonlocal stopped
        stopped = True
        if current is not None and current.poll() is None:
            try:
                os.killpg(current.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    signal.signal(signal.SIGINT, cancel)
    signal.signal(signal.SIGTERM, cancel)
    state = {e["id"]: {engine: {"status": "pending", "used_seconds": 0,
                                "completed_repetitions": 0} for engine in engines} for e in entries}
    run = {"schema": "public-synthetic-run-v1", "historical_measurement": False,
           "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
           "cpus": cpus, "engines": engines, "manifest_sha256": digest(mpath),
           "budget_seconds": args.budget_seconds, "entry_mode_total_seconds": args.entry_seconds,
           "address_space_bytes": 2 * 1024 ** 3, "warm_count": count,
           "warm_sample_kind": "singleton" if count == 1 else "batch mean per input, helpers drained after each input",
           "variant": args.variant, "quiet_marpa": args.quiet_marpa,
           "source_sha256": digest(Path(__file__)), "selected_ids": list(state), "states": state,
           "crank_build_receipt": build_receipt,
           "crank_build_receipt_sha256": digest(build_path) if build_receipt else None,
           "marpa_runner_sha256": marpa_hash,
           "observed_marpa_receipt_versions": []}
    if "marpa" in engines:
        probe = 'use Marpa::R2; use JSON::PP; print JSON::PP->new->encode({module_version=>$Marpa::R2::STRING_VERSION,libmarpa_version=>join(q{.},Marpa::R2::Thin::version()),perl_version=>qq{$^V}});'
        try:
            observed = subprocess.run(["perl", "-e", probe], capture_output=True, text=True, timeout=min(5, args.budget_seconds))
            run["marpa_version_probe"] = {"exit_code": observed.returncode,
                    "versions": json.loads(observed.stdout) if observed.returncode == 0 else None}
        except (OSError, ValueError, subprocess.TimeoutExpired):
            run["marpa_version_probe"] = {"versions": None, "status": "unavailable_or_invalid_probe"}
    receipts = collections.defaultdict(list)

    def checkpoint():
        temp = args.output / "run.json.tmp"
        temp.write_text(json.dumps(run, indent=2) + "\n")
        temp.replace(args.output / "run.json")

    checkpoint()
    with (args.output / "raw.jsonl").open("w", buffering=1) as journal:
        for index, entry in enumerate(entries):
            # Reserve every mode's full cumulative allowance; no shared shrinking timeout.
            if stopped or deadline - time.monotonic() < len(engines) * args.entry_seconds + 1:
                run["stop_reason"] = "cancelled" if stopped else "insufficient_full_group_budget"
                break
            if shutil.disk_usage(args.output).free < 3 * 1024 ** 3:
                run["stop_reason"] = "disk_reserve"
                break
            for rep in range(repeats):
                order = engines[(index + rep) % len(engines):] + engines[:(index + rep) % len(engines)]
                for engine in order:
                    st = state[entry["id"]][engine]
                    if stopped or st["status"] in FAILURES:
                        continue
                    allowance = args.entry_seconds - st["used_seconds"]
                    if allowance <= 0:
                        st["status"] = "timeout"
                        continue
                    grammar = mpath.parent / entry["grammars"]["marpa" if engine == "marpa" else "crank"]["path"]
                    corpus = mpath.parent / entry["corpora"][rep]["path"]
                    if digest(marpa) != marpa_hash or (engine != "marpa" and digest(binary) != build_receipt["binary_sha256"]):
                        run["stop_reason"] = "frontend_changed_after_admission"
                        checkpoint()
                        raise SystemExit("frontend bytes changed; campaign stopped")
                    command = (["perl", str(marpa)] if engine == "marpa" else [str(binary)]) + [engine, str(grammar), str(corpus), "measure", str(count), str(samples), "1"]
                    mask = set(cpus if engine == "progress4" else cpus[:1])

                    def limits():
                        os.sched_setaffinity(0, mask)
                        os.nice(10)
                        resource.setrlimit(resource.RLIMIT_AS, (2 * 1024 ** 3, 2 * 1024 ** 3))
                        resource.setrlimit(resource.RLIMIT_FSIZE, (32 * 1024 ** 2, 32 * 1024 ** 2))

                    stem = args.output / f"{entry['id']}-{engine}-{rep}"
                    child_start = time.monotonic()
                    timed_out = False
                    with stem.with_suffix(".stdout").open("wb") as stdout, stem.with_suffix(".stderr").open("wb") as stderr:
                        try:
                            current = subprocess.Popen(command, stdout=stdout, stderr=stderr, preexec_fn=limits, start_new_session=True)
                            try:
                                code = current.wait(timeout=allowance)
                            except subprocess.TimeoutExpired:
                                timed_out = True
                                os.killpg(current.pid, signal.SIGKILL)
                                code = current.wait()
                        except FileNotFoundError:
                            code = 127
                        finally:
                            current = None
                    duration = time.monotonic() - child_start
                    st["used_seconds"] += duration
                    try:
                        if stem.with_suffix(".stdout").stat().st_size > 4 * 1024 ** 2:
                            raise ValueError("stdout receipt exceeds bound")
                        receipt = json.loads(stem.with_suffix(".stdout").read_text())
                    except (ValueError, UnicodeError):
                        receipt = {"status": "unavailable" if code == 127 else "protocol_error"}
                    status = classify(code, receipt, timed_out, stopped)
                    if status == "ok":
                        try:
                            validate(receipt, engine, entry["expected"], 1 + count * samples, count, samples)
                        except ValueError as exc:
                            status = "protocol_error"
                            receipt["validation_error"] = str(exc)
                    if engine == "marpa" and receipt.get("module_version"):
                        observed_version = {k: receipt[k] for k in ("module_version", "perl_version") if k in receipt}
                        if observed_version not in run["observed_marpa_receipt_versions"]:
                            run["observed_marpa_receipt_versions"].append(observed_version)
                    stderr_path = stem.with_suffix(".stderr")
                    with stderr_path.open("rb") as f:
                        warning_lines = sum(1 for line in f if b"warning" in line.lower())
                    row = {"id": entry["id"], "engine": engine, "repeat": rep, "status": status,
                           "exit_code": code, "child_wall_seconds": duration,
                           "stderr_bytes": stderr_path.stat().st_size, "warning_lines": warning_lines,
                           "receipt": receipt}
                    journal.write(json.dumps(row) + "\n")
                    st["status"] = status
                    if status == "ok":
                        st["completed_repetitions"] += 1
                        receipts[(entry["id"], engine)].append(receipt)
                        if st["completed_repetitions"] == repeats:
                            st["status"] = "complete"
                            st.update(costs(receipts[(entry["id"], engine)], count))
                    checkpoint()
    run.setdefault("stop_reason", "cancelled" if stopped else "finished")
    run["elapsed_seconds"] = time.monotonic() - started
    checkpoint()
    print(json.dumps({"stop_reason": run["stop_reason"], "elapsed_seconds": run["elapsed_seconds"],
                      "mode_entry_status_counts": dict(collections.Counter(x["status"] for e in state.values() for x in e.values()))}))


if __name__ == "__main__":
    main()
