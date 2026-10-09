#!/usr/bin/env python3
"""Regenerate the original September 11 practical latency graph from archived data.

Plot functions/constants below are unchanged excerpts of the supplied
marpa_revisit/analyze.py. The portable loader verifies the compact input hash;
it performs no builds, recognizer queries, downloads, or new measurements.
PNG bytes may differ with Matplotlib/font versions; original PNG/SVG are retained.
"""
from pathlib import Path
import argparse
import hashlib
import json
import math

PLOT_MODES = ("native", "plan1", "parallel4", "default4", "marpa")


COMPLETE = {"match", "no_match"}


ROOT_PROOFS = {"length", "first", "last", "first_pair", "last_pair", "nullable"}


LABELS = {"native": "Crank native", "plan1": "Crank plan, 1 worker",
          "parallel4": "Crank plan, 4 workers", "parallel8": "Crank plan, 8 workers",
          "default4": "Crank default grain, 4 workers", "marpa": "Marpa",
          "native_unpruned": "Crank without pruning", "marpa_filter": "Crank root filter + Marpa"}


COLORS = {"native": "#157F9F", "plan1": "#D88126", "parallel4": "#4056A1",
          "parallel8": "#9467BD", "default4": "#668A65", "marpa": "#C04D61"}


FAMILY_LABELS = {"json_rows": "JSON row document", "json_deep": "Deep JSON nesting",
                 "event_stream": "Nested event stream", "http_headers": "HTTP-like headers",
                 "filter_expression": "Filter expression DSL", "nested_tags": "Nested a/b tags",
                 "fixed_record_batch": "Fixed-record ingestion",
                 "independent_branches": "Independent recursive branches",
                 "shared_prefix": "One shared recursive prefix", "mixed_prefix": "Four shared-prefix groups",
                 "count_gcd": "Repetition GCD", "count_bounded": "Bounded repetition counts",
                 "count_lattice": "Repetition integer lattice"}


PRACTICAL_FAMILIES = ("json_rows", "event_stream", "http_headers", "filter_expression",
                      "nested_tags", "fixed_record_batch", "json_deep")


VARIANT_ORDER = {"valid": 0, "accept": 0, "early_byte": 1, "middle_order": 2,
                 "late_order": 3, "length_plus_one": 4}


VARIANT_LABELS = {"valid": "Valid", "accept": "Valid", "early_byte": "Early",
                  "middle_order": "Middle", "late_order": "Late", "length_plus_one": "Length +1"}


def complete(row):
    return bool(row) and row.get("timing_kind") == "completed_query" and all(
        status in COMPLETE for status in row.get("statuses", [])) and row.get("median_us") is not None


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def outcome_label(row):
    if row is None:
        return "MISSING"
    statuses = set(row.get("statuses", []))
    names = {"limit": "LIMIT", "timeout": "TIMEOUT", "memory": "MEMORY",
             "compile_rejected": "UNSUPPORTED", "match": "MATCH", "no_match": "NO MATCH"}
    return "/".join(names.get(status, status.upper()) for status in sorted(statuses)) or "MISSING"


def practical_large(case):
    # Most generators floor a target length to a whole number of records, so
    # their scale=8192 subjects are just under 8192 bytes. Include that entire
    # predeclared target class as well as subjects actually at least 8192 bytes.
    return bool(case["practical"]) and (case["scale"] >= 8192 or case["bytes"] >= 8192)


def setup_plotting():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "axes.titleweight": "bold", "axes.titlesize": 11,
                         "svg.fonttype": "none", "svg.hashsalt": "crank-marpa-revisit"})
    return plt


def save_figure(fig, stem, plt):
    fig.savefig(stem.with_suffix(".png"), dpi=180, bbox_inches="tight", facecolor="white")
    fig.savefig(stem.with_suffix(".svg"), bbox_inches="tight", facecolor="white", metadata={"Date": None})
    plt.close(fig)


def draw_practical(output, cases, by_case_mode, reasons):
    plt = setup_plotting()
    selected = [case for case in cases if practical_large(case)]
    families = [family for family in PRACTICAL_FAMILIES if any(case["family"] == family for case in selected)]
    families += sorted({case["family"] for case in selected} - set(families))
    if not families:
        return []
    nrows = math.ceil((len(families) + 1) / 2)
    fig, axes = plt.subplots(nrows, 2, figsize=(13.4, 3.15 * nrows), squeeze=False)
    values = [row["median_us"] for case in selected for mode in PLOT_MODES
              if complete(row := by_case_mode.get((case["id"], mode))) and row["median_us"] > 0]
    bottom = 10 ** math.floor(math.log10(min(values))) if values else .01
    top = 10 ** math.ceil(math.log10(max(values))) if values else 1e5
    top = max(top, bottom * 100)
    for panel, family in enumerate(families):
        ax = axes.flat[panel]
        members = sorted((case for case in selected if case["family"] == family), key=lambda case: (case["scale"], VARIANT_ORDER.get(case["variant"], 99), case["variant"]))
        multiple_scales = len({case["scale"] for case in members}) > 1
        labels = []
        for index, case in enumerate(members):
            filtered = not case["expected"] and reasons[case["id"]] in ROOT_PROOFS
            label = VARIANT_LABELS.get(case["variant"], case["variant"].replace("_", " "))
            if multiple_scales:
                label += f"\n{case['scale']}"
            if filtered:
                label += " †"
                ax.axvspan(index - .48, index + .48, color="#EEF6E8", zorder=0)
            labels.append(label)
            for mode_index, mode in enumerate(PLOT_MODES):
                row = by_case_mode.get((case["id"], mode))
                x = index + (mode_index - 2) * .135
                if complete(row) and row["median_us"] > 0:
                    point = row["median_us"]
                    replicates = [value for value in row.get("median_us_replicates", []) if number(value) and value > 0]
                    low, high = min(replicates + [point]), max(replicates + [point])
                    ax.errorbar(x, point, yerr=[[point - low], [high - point]], fmt="o", markersize=5.2,
                                capsize=2, linewidth=.9, color=COLORS[mode], zorder=3)
                else:
                    ax.text(x, .97 - mode_index * .073, outcome_label(row), transform=ax.get_xaxis_transform(),
                            color=COLORS[mode], fontsize=6.5, ha="center", va="top", rotation=90)
        byte_values = [case["bytes"] for case in members]
        byte_text = str(min(byte_values)) if min(byte_values) == max(byte_values) else f"{min(byte_values)}–{max(byte_values)}"
        ax.set_title(f"{FAMILY_LABELS.get(family, family)}  ·  {byte_text} bytes", loc="left", pad=10)
        ax.set_xticks(range(len(members)), labels)
        ax.set_xlim(-.6, len(members) - .4)
        ax.set_yscale("log")
        ax.set_ylim(bottom / 1.35, top * 1.5)
        ax.set_ylabel("Warm query latency (µs)")
        ax.grid(axis="y", which="major", alpha=.2)
        ax.tick_params(axis="x", labelsize=9)
    legend_ax = axes.flat[len(families)]
    legend_ax.axis("off")
    for mode in PLOT_MODES:
        legend_ax.plot([], [], "o", color=COLORS[mode], label=LABELS[mode])
    legend_ax.legend(loc="upper left", frameon=False, fontsize=10)
    legend_ax.text(.02, .43, "Lower is faster. All axes use the same log scale.\n"
        "Dots: median of fresh-process medians.\nWhiskers: observed replicate range, not a confidence interval.\n"
        "† Shading: native root filter proves this rejection.\n"
        "LIMIT/other labels: no completed comparison; no point plotted.\n"
        "Plan 4 forces task grain 0; Default 4 retains 4096 bytes.\n"
        "Configured workers can exceed the number actually used.", transform=legend_ax.transAxes,
        fontsize=8.5, va="top", linespacing=1.30)
    for index in range(len(families) + 1, nrows * 2):
        axes.flat[index].axis("off")
    fig.suptitle("Crank and Marpa on every approximately 8 KiB practical fixture", fontsize=15, fontweight="bold", y=1.005)
    fig.tight_layout(h_pad=2.5, w_pad=2.5)
    save_figure(fig, output / "practical_overview", plt)
    return [case["id"] for case in selected]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=Path(__file__).with_name("practical_graph_data.json"))
    parser.add_argument("--output", type=Path, default=Path.cwd() / "build" / "benchmark-figures")
    args = parser.parse_args()
    payload = args.data.read_bytes()
    expected = "0a69ffd37f1b41390bdf4797d5b8755ddbfb16e9322c49657e542d09a6d0293a"
    if hashlib.sha256(payload).hexdigest() != expected:
        raise SystemExit("Archived plotting data hash mismatch")
    data = json.loads(payload)
    pairs = {(row["id"], row["mode"]): row for row in data["rows"]}
    if len(data["cases"]) != 28 or len(pairs) != 140 or len(data["rows"]) != 140:
        raise SystemExit("Archived graph requires exactly 28 cases and 140 plotted points")
    if any(not complete(row) or row["completed_replicates"] != 3 for row in data["rows"]):
        raise SystemExit("A plotted case is incomplete")
    if any(len(row["median_us_replicates"]) != 3 for row in data["rows"]):
        raise SystemExit("Every plotted point requires three original replicate medians")
    args.output.mkdir(parents=True, exist_ok=True)
    selected = draw_practical(args.output, data["cases"], pairs, data["root_filter_reasons"])
    if set(selected) != {case["id"] for case in data["cases"]}:
        raise SystemExit("Graph selection differs from the archived figure")
    print(f"Regenerated historical graph from {len(selected)} cases; no benchmarks run.")
    print(args.output / "practical_overview.png")
    print(args.output / "practical_overview.svg")


if __name__ == "__main__":
    main()
