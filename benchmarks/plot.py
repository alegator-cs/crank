#!/usr/bin/env python3
"""Plot completed current measurements; nondecisions never become zero latency."""
import argparse
import json
from pathlib import Path
import statistics


def summarize(report):
    if report.get("check_only") or report.get("gate") != "passed for completed checks; nondecisions excluded from timing":
        raise ValueError("only a completed correctness-gated measurement campaign can be plotted")
    if not report.get("finished_utc"):
        raise ValueError("campaign is unfinished")
    result = []
    pairs = sorted({(r["fixture"], r["engine"]) for r in report["measurements"]})
    for fixture, engine in pairs:
        rows = [r for r in report["measurements"] if (r["fixture"], r["engine"]) == (fixture, engine)]
        complete = [r for r in rows if r["status"] == "complete"]
        entry = dict(fixture=fixture, engine=engine, completed_processes=len(complete),
                     requested_processes=report["measurement"]["repeats"],
                     statuses={s: sum(r["status"] == s for r in rows) for s in sorted({r["status"] for r in rows})})
        if complete:
            warm = [statistics.median(r["warm_wall_ns_per_query"]) for r in complete]
            cold = [r["prepare_ns"] + r["first_query_ns"] for r in complete]
            entry.update(warm_process_medians_ns=warm, cold_process_ns=cold,
                         warm_median_ns=statistics.median(warm) if len(complete) == entry["requested_processes"] else None,
                         cold_median_ns=statistics.median(cold) if len(complete) == entry["requested_processes"] else None,
                         minimum_recorded_batch_ns=min(min(r["warm_batch_ns"]) for r in complete),
                         calibration_capped_processes=sum(r.get("calibration_capped", False) for r in complete))
        result.append(entry)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path, help="figure stem, default beside results")
    args = parser.parse_args()
    report = json.loads(args.results.read_text())
    summary = summarize(report)
    stem = args.output or args.results.parent / "current"
    stem.parent.mkdir(parents=True, exist_ok=True)
    stem.with_name(stem.name + "-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        raise SystemExit("Chart dependency unavailable: Matplotlib; measurements and summary were saved")
    ids = [f["id"] for f in report["fixtures"]]
    engines = list(dict.fromkeys(r["engine"] for r in report["measurements"]))
    fig, axes = plt.subplots(1, 2, figsize=(15, max(5, len(ids) * .43)), sharey=True)
    colors = plt.get_cmap("tab10")
    offsets = [(i - (len(engines) - 1) / 2) * .7 / max(1, len(engines)) for i in range(len(engines))]
    lookup = {(r["fixture"], r["engine"]): r for r in summary}
    if set(lookup) != {(fixture, engine) for fixture in ids for engine in engines}:
        raise ValueError("measurement fixture/engine grid is incomplete")
    for i, engine in enumerate(engines):
        for axis, metric in zip(axes, ("cold_median_ns", "warm_median_ns")):
            points = [(j + offsets[i], lookup[(fixture, engine)].get(metric)) for j, fixture in enumerate(ids)]
            points = [(y, x / 1000) for y, x in points if x is not None and x > 0]
            if points:
                axis.plot([x for y, x in points], [y for y, x in points], "o", color=colors(i), label=engine, markersize=4)
    for axis, title in zip(axes, ("Preparation + first query", "Warm reuse: median of process medians")):
        axis.set_xscale("log"); axis.set_xlabel("microseconds, logarithmic scale")
        axis.set_title(title); axis.grid(axis="x", which="both", alpha=.25)
    axes[0].set_yticks(range(len(ids)), ids); axes[0].invert_yaxis()
    handles, labels = axes[1].get_legend_handles_labels()
    if handles: axes[1].legend(handles, labels, loc="best")
    nondecisions = sum(r["status"] != "complete" for r in report["measurements"])
    partial = sum(0 < r["completed_processes"] < r["requested_processes"] for r in summary)
    fig.suptitle("Current full-span byte membership - " + report["suite"])
    fig.text(.02, .007, f"Fresh run: {report['created_utc']} - {nondecisions} nondecision rows and {partial} partial pairs omitted. "
             f"See {args.results.name} and {stem.name}-summary.json for statuses, batch durations and build provenance.", fontsize=8)
    fig.tight_layout(rect=(0, .025, 1, .97))
    for suffix in (".svg", ".png"):
        fig.savefig(stem.with_suffix(suffix), dpi=160)
    plt.close(fig)
    print(stem.with_suffix(".svg"))


if __name__ == "__main__":
    main()
