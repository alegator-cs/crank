#!/usr/bin/env python3
"""Freeze public synthetic inputs; this performs no engine measurement."""
import argparse
import hashlib
import json
import shutil
import struct
from pathlib import Path

from contracts import additional, ascii_json, long, practical

GRAMMARS = {**practical.CRANK_GRAMMARS, **additional.CRANK_GRAMMARS, "json_ascii": ascii_json.CRANK}
MARPA = {**practical.MARPA_GRAMMARS, **additional.MARPA_GRAMMARS, "json_ascii": ascii_json.MARPA}
SCOPES = {**practical.SCOPES, **additional.SCOPES, "json_ascii": ascii_json.SCOPE}
FAMILIES = ["json_ascii" if f == "restricted_json" else f for f in practical.FAMILY_ORDER] + list(additional.CRANK_GRAMMARS)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def accepts(family, text):
    if family == "restricted_json":
        return practical.json_accepts(text)
    if family == "json_ascii":
        return ascii_json.accepts(text)
    return long.accepts(family, text)


def fresh(entry, index):
    marker = entry["fresh_marker"]
    if index == 0:
        return entry["subject"]
    value, letters = index - 1, []
    for _ in marker:
        letters.append(chr(97 + value % 26))
        value //= 26
    if value:
        raise ValueError("fresh marker exhausted")
    result = entry["subject"].replace(marker, "".join(letters), 1)
    if len(result) != len(entry["subject"]) or accepts(entry["family"], result) != entry["expected"]:
        raise ValueError("fresh variant changed the language contract")
    return result


def entries(suite):
    if suite != "wide":
        rows = practical.entries() + additional.cases()
        markers = {"csv": "case", "ini": "PPI", "xml_subset": "alpha0000",
                   "arithmetic_expression": "var000"}
        for row in rows:
            row.setdefault("fresh_marker", markers.get(row["family"]))
            row["provenance"] = "synthetic_public_fixture"
        return [r for r in rows if suite != "smoke" or r["size"] == "small"]
    rows = []
    for size, target, depth in [("medium", 64 * 1024, 32), ("large", 512 * 1024, 128)]:
        for family in FAMILIES:
            requested = (256 if size == "medium" else 1024) if family == "regex_syntax" else target
            marker = "route" if family == "event_stream" else "alpha"
            if family == "json_ascii":
                value = {"benchmark_marker": "alpha", "records": []}
                # Fixed construction, independent of either engine's results.
                value["records"] = [{"name": "record%05d" % i, "value": i,
                                      "enabled": True, "items": [1, 2, None]}
                                     for i in range(max(1, target // 74))]
                good = practical.restricted_serialization(value)
            else:
                good, _ = long.positive(family, requested, depth, additional.CSV_SEED, additional.INI_SEED)
            if not accepts(family, good):
                raise ValueError((family, "invalid positive"))
            marker_pos = good.index(marker)
            cases = [("accepted", True, good, None)]
            for outcome, fraction in [("early_rejected", .01), ("middle_rejected", .5), ("late_rejected", .99)]:
                if family == "json_ascii":
                    offset = int(fraction * (len(good) - 1))
                    if marker_pos <= offset < marker_pos + len(marker):
                        offset = marker_pos + len(marker)
                    bad = good[:offset] + "\x00" + good[offset + 1:]
                else:
                    bad, offset, _ = long.malformed(family, good, fraction, marker_pos)
                if accepts(family, bad):
                    raise ValueError("malformed fixture passed its oracle")
                cases.append((outcome, False, bad, offset))
            for outcome, expected, text, offset in cases:
                rows.append({"id": f"{family}_{size}_{outcome}", "family": family,
                             "size": size, "subject": text, "bytes": len(text),
                             "expected": expected, "fresh_marker": marker,
                             "mutation_offset": offset, "scope": SCOPES[family],
                             "provenance": "synthetic_public_fixture",
                             "planned_weight": 1 / 32 if expected else 1 / 96})
    return rows


def cfi(subjects, expected):
    result = bytearray(b"CFI1" + struct.pack("<I", len(subjects)))
    for text in subjects:
        raw = text.encode("ascii")
        result += struct.pack("<IB", len(raw), int(expected)) + raw
    return bytes(result)


def generate(destination, suite="smoke", count=1, samples=7, repeats=3):
    if destination.exists():
        raise ValueError("use a new output directory; frozen inputs are never overwritten")
    rows = entries(suite)
    records = 1 + count * samples
    estimated = sum((r["bytes"] + 5) * records * repeats for r in rows)
    if estimated > 512 * 1024 ** 2:
        raise ValueError("requested corpus exceeds the 512 MiB generator bound")
    if shutil.disk_usage(destination.parent).free < 3 * 1024 ** 3 + estimated:
        raise ValueError("generation would leave less than 3 GiB free")
    destination.mkdir()
    manifest = {"schema": "public-synthetic-corpus-v1", "suite": suite,
                "historical_measured_inputs": False, "engines_run": False,
                "warm_count": count, "warm_samples": samples, "repeats": repeats,
                "semantics": "ASCII Boolean whole-input recognition, no semantic actions/captures/forest",
                "selection": "Fixed family/size/outcome construction before timing; smoke is the small subset",
                "entries": [], "source_bindings": {}}
    for module in [Path(__file__), *sorted((Path(__file__).parent / "contracts").glob("*.py"))]:
        manifest["source_bindings"][module.name] = sha(module.read_bytes())
    grammar_provenance = Path(__file__).parent / "GRAMMAR_SOURCES.json"
    manifest["source_bindings"][grammar_provenance.name] = sha(grammar_provenance.read_bytes())
    manifest["grammar_provenance"] = json.loads(grammar_provenance.read_text())
    for row in rows:
        item = {k: v for k, v in row.items() if k != "subject"}
        item["corpora"] = []
        item["grammars"] = {}
        for ext, text in [("crank", GRAMMARS[row["family"]]), ("marpa", MARPA[row["family"]])]:
            path = destination / "grammars" / (row["family"] + "." + ext)
            path.parent.mkdir(exist_ok=True)
            data = text.encode("ascii")
            path.write_bytes(data)
            item["grammars"][ext] = {"path": path.relative_to(destination).as_posix(), "sha256": sha(data)}
        for rep in range(repeats):
            subjects = [fresh(row, rep * records + i) for i in range(records)]
            if len(set(subjects)) != records:
                raise ValueError("duplicate fresh inputs")
            if any(accepts(row["family"], s) != row["expected"] for s in subjects):
                raise ValueError("independent oracle mismatch")
            blob = cfi(subjects, row["expected"])
            path = destination / "corpora" / row["id"] / f"rep-{rep}.cfi"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(blob)
            item["corpora"].append({"path": path.relative_to(destination).as_posix(), "sha256": sha(blob),
                                    "records": records, "input_sha256": [sha(s.encode("ascii")) for s in subjects]})
        manifest["entries"].append(item)
    path = destination / "manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf8")
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", type=Path)
    ap.add_argument("--suite", choices=["smoke", "practical", "wide"], default="smoke")
    ap.add_argument("--count", type=int, choices=[1, 128], default=1)
    ap.add_argument("--samples", type=int, default=7)
    ap.add_argument("--repeats", type=int, default=3)
    args = ap.parse_args()
    if not 1 <= args.samples <= 7 or not 1 <= args.repeats <= 3:
        ap.error("samples must be 1..7 and repeats 1..3")
    manifest = generate(args.output.resolve(), args.suite, args.count, args.samples, args.repeats)
    print(json.dumps({"entries": len(manifest["entries"]), "engines_run": False,
                      "historical_measured_inputs": False}))


if __name__ == "__main__":
    main()
