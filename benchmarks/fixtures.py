#!/usr/bin/env python3
"""Deterministic byte fixtures with independent, small membership oracles."""
import hashlib
import json
from pathlib import Path
from src.practical_oracles import validate_cases, ORACLES
from src.generate_practical import practical_cases as generate_practical

HERE = Path(__file__).resolve().parent


def practical_cases():
    rows = generate_practical()
    provenance = json.loads((HERE / "data" / "practical-provenance.json").read_text())
    expected = {row["id"]: row for row in provenance["cases"]}
    if len(rows) != 28 or {row["id"] for row in rows} != set(expected):
        raise ValueError("practical case identity differs from original figure")
    validate_cases(rows)
    result = []
    for row in rows:
        pattern, subject = row["pattern"].encode("latin-1"), row["subject"].encode("latin-1")
        for key, value in (("pattern_sha256", pattern), ("subject_sha256", subject)):
            if hashlib.sha256(value).hexdigest() != expected[row["id"]][key]:
                raise ValueError("practical fixture byte hash mismatch: " + row["id"])
        result.append(dict(id=row["id"], group=row["family"], pattern=pattern, subject=subject,
                           expected=row["expected"], oracle=row["oracle"],
                           scope=row.get("scope", ""), historical_campaign="2026-09-11"))
    return result


def assistance_cases():
    archived = json.loads((HERE / "data" / "assistance-fixtures.json").read_text())["fixtures"]
    result = []
    for row in archived:
        if row["id"] == "mixed_prefix_late_marker_reject_4096":
            subject = b"a" * 4096 + b"b" * 4096 + b"ccZdd"
            positive = subject[:-3] + b"Add"
            oracle = lambda s: s == b"a" * 4096 + b"b" * 4096 + b"ccAdd"
        elif row["id"] == "event_stream_131072_late_order":
            record = b'E[id=17;msg="route";child=E[id=2;msg="ok"];samples=[1,2,3]]\n'
            positive = record * (131072 // len(record))
            at = positive.rfind(b"=[")
            subject = positive[:at] + b"[=" + positive[at + 2:]
            oracle = lambda s: ORACLES["event_recursive_descent"](s.decode("latin-1"), {})
        else:
            raise ValueError("unknown archived assistance fixture")
        pattern = row["pattern"].encode("latin-1")
        if hashlib.sha256(subject).hexdigest() != row["subject_sha256"] or hashlib.sha256(pattern).hexdigest() != row["pattern_sha256"]:
            raise ValueError("assistance fixture differs from historical byte hashes")
        for name, value in ((row["id"], subject), (row["id"] + "_positive", positive)):
            expected = oracle(value)
            if expected != name.endswith("_positive"):
                raise ValueError("independent assistance oracle disagrees")
            result.append(dict(id=name, group="assistance", pattern=pattern, subject=value,
                               expected=expected, oracle=row["oracle"], historical_campaign=row["campaign"],
                               scope=row["language_scope"]))
    return result


def lattice_possible(length, bound=None):
    top = length // 131 if bound is None else min(bound, length // 131)
    for y in range(top + 1):
        rest = length - 131 * y
        if rest % 127 == 0 and (bound is None or rest // 127 <= bound):
            return True
    return False


def cases(suite="all"):
    result = []

    def add(name, group, pattern, subject, expected, oracle):
        if not isinstance(expected, bool):
            raise ValueError("fixture verdict must be Boolean")
        result.append(dict(id=name, group=group, pattern=pattern.encode("ascii"),
                           subject=subject, expected=expected, oracle=oracle))

    if suite in ("smoke", "all", "cfg"):
        n = 8 if suite == "smoke" else 64
        pattern = r"(?<S>a(?&S)b|)"
        add("balanced_valid", "cfg", pattern, b"a" * n + b"b" * n, True,
            "exact language a^n b^n")
        add("balanced_invalid", "cfg", pattern, b"a" * n + b"b" * (n - 1), False,
            "unequal a/b counts")
        add("regular_valid", "regular", "(?:ab)+", b"ab" * n, True,
            "literal ab copies")
        add("regular_invalid", "regular", "(?:ab)+", b"ab" * n + b"a", False,
            "incomplete final literal")

    if suite in ("rejection", "all"):
        unbounded = "(?:a{127})*(?:a{131})*"
        bounded = "(?:a{127}){0,63}(?:a{131}){0,63}"
        for name, pattern, length, limit in (
            ("lattice_16379", unbounded, 16379, None),
            ("lattice_16380_positive", unbounded, 16380, None),
            ("bounded_lattice_8191", bounded, 8191, 63),
            ("bounded_lattice_8192_positive", bounded, 8192, 63),
            ("bounded_lattice_16379", bounded, 16379, 63),
            ("bounded_lattice_16254_positive", bounded, 16254, 63),
        ):
            add(name, "rejection", pattern, b"a" * length,
                lattice_possible(length, limit),
                "exhaustive nonnegative 127*x+131*y=length; both counts <=63 when bounded")
        good = b"ABCD:123456\n" * (131072 // 12)
        pattern = r"(?:[A-Z]{4}:[0-9]{6}\n)+"
        add("record_flat_131072_final_digit", "rejection", pattern, good[:-2] + b"!\n",
            False, "final six-digit field contains !; actual byte length 131064")
        add("record_flat_131072_positive", "rejection", pattern, good, True,
            "10922 exact twelve-byte records; actual byte length 131064")

    if suite in ("practical", "all"):
        result.extend(practical_cases())
    if suite in ("assistance", "all"):
        result.extend(assistance_cases())
    if not result:
        raise ValueError("unknown or empty suite: " + suite)
    return result
