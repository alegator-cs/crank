#!/usr/bin/env python3
"""Recompute historical weighted indices from public CSVs without engines."""
import csv
import hashlib
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent


def close(actual, expected):
    if not math.isclose(actual, expected, rel_tol=1e-9, abs_tol=1e-12):
        raise ValueError(f"metric differs: {actual} versus {expected}")


def verify(directory=HERE / "results"):
    provenance = json.loads((directory / "PROVENANCE.json").read_text())
    for name, expected in provenance.get("ancillary_results", {}).items():
        if hashlib.sha256((directory / name).read_bytes()).hexdigest() != expected:
            raise ValueError("ancillary audit hash mismatch")
    results = {}
    for phase, binding in provenance["phases"].items():
        path = directory / (phase + ".csv")
        if hashlib.sha256(path.read_bytes()).hexdigest() != binding["public_csv_sha256"]:
            raise ValueError("public CSV hash mismatch")
        summary_path = directory / (phase + ".json")
        if hashlib.sha256(summary_path.read_bytes()).hexdigest() != binding["public_summary_sha256"]:
            raise ValueError("public summary hash mismatch")
        rows = list(csv.DictReader(path.open(newline="", encoding="utf8")))
        if len(rows) != binding["rows"]:
            raise ValueError("row count differs")
        indexed = {(r["id"], r["engine"]): r for r in rows}
        if len(indexed) != len(rows):
            raise ValueError("duplicate mode/entry")
        summary = json.loads(summary_path.read_text())
        for engine, expected in summary["comparisons"].items():
            pairs = [(r, indexed[(r["id"], "marpa")]) for r in rows if r["engine"] == engine
                     and r["mode_complete"] == "True"
                     and indexed[(r["id"], "marpa")]["mode_complete"] == "True"]
            mass = sum(float(r.get("planned_weight") or 1 / summary["planned_count"]) for r, _ in pairs)
            if len(pairs) != expected["completed_pairs"]:
                raise ValueError("paired count differs")
            close(mass, expected["completed_planned_weight"])
            wins = sum(float(r["warm_wall_us"]) < float(m["warm_wall_us"]) for r, m in pairs)
            if wins != expected["warm_wall_wins"]:
                raise ValueError("win count differs")
            for clock in ("wall", "cpu"):
                weighted = [(float(r.get("planned_weight") or 1 / summary["planned_count"]),
                             float(r["warm_" + clock + "_us"]) / float(m["warm_" + clock + "_us"]))
                            for r, m in pairs]
                geometric = math.exp(sum(w * math.log(q) for w, q in weighted) / mass)
                arithmetic = sum(w * q for w, q in weighted) / mass
                close(geometric, expected["explicitly_renormalized_completed_subset_geomean_crank_over_marpa"][clock])
                close(arithmetic, expected["explicitly_renormalized_completed_subset_arithmetic_mean_crank_over_marpa"][clock])
                full = expected["full_corpus_geomean_crank_over_marpa"][clock]
                if len(pairs) == summary["planned_count"]:
                    close(geometric, full)
                elif full is not None:
                    raise ValueError("partial coverage has a full index")
            results[phase + "/" + engine] = {"pairs": len(pairs), "wins": wins, "coverage_weight": mass}
    return results


if __name__ == "__main__":
    print(json.dumps(verify(), indent=2))
