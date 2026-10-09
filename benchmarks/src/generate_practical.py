#!/usr/bin/env python3
"""Reconstruct the exact 28 inputs plotted in the 2026-09-11 practical graph.

This is a fixture-only translation of marpa_revisit/practical_cases.cpp; it does
not run either recognizer. Every emitted byte string is checked against hashes
from the original cases.json. Patterns and language scopes are archived metadata.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent


def _copies(target: int, overhead: int, width: int) -> int:
    return max(1, (target - overhead if target > overhead else 1) // width)


def _swap(text: str, first: int, second: int) -> str:
    result = list(text)
    result[first], result[second] = result[second], result[first]
    return "".join(result)


def _find(text: str, needle: str, start: int = 0) -> int:
    at = text.find(needle, start)
    if at < 0:
        raise ValueError("fixture marker missing")
    return at


def _rfind(text: str, needle: str) -> int:
    at = text.rfind(needle)
    if at < 0:
        raise ValueError("fixture marker missing")
    return at


def _subjects() -> dict[str, str]:
    result: dict[str, str] = {}
    scale = 8192

    def emit(family: str, variant: str, text: str, size: int = scale) -> None:
        result[f"{family}_{size}_{variant}"] = text

    # Original practical_cases.cpp lines 101-122.
    row = r'{"id":17,"ok":true,"x":[-12.5e+2,null,"line\n\u0041"]}'
    text = '{"rows":[' + ','.join([row] * _copies(scale, 11, len(row) + 1)) + ']}'
    emit("json_rows", "valid", text)
    emit("json_rows", "early_byte", "!" + text[1:])
    at = _find(text, ":", len(text) // 2)
    emit("json_rows", "middle_order", _swap(text, at, at + 1))
    at = _rfind(text, ":")
    emit("json_rows", "late_order", _swap(text, at, at + 1))

    # Original practical_cases.cpp lines 125-143.
    row = 'E[id=17;msg="route";child=E[id=2;msg="ok"];samples=[1,2,3]]\n'
    text = row * _copies(scale, 0, len(row))
    emit("event_stream", "valid", text)
    emit("event_stream", "early_byte", "!" + text[1:])
    at = _find(text, ";samples=[", len(text) // 2) + 8
    emit("event_stream", "middle_order", _swap(text, at, at + 1))
    at = _rfind(text, ";samples=[") + 8
    emit("event_stream", "late_order", _swap(text, at, at + 1))

    # Original practical_cases.cpp lines 146-164.
    row = "X-Trace: abcd0123abcd0123\r\n"
    text = "GET /api/events?batch=1 HTTP/1.1\r\nHost: example.test\r\n"
    text += row * _copies(scale, len(text) + 2, len(row)) + "\r\n"
    emit("http_headers", "valid", text)
    emit("http_headers", "early_byte", "!" + text[1:])
    at = _find(text, ":", len(text) // 2)
    emit("http_headers", "middle_order", _swap(text, at, at + 1))
    at = _rfind(text, ":")
    emit("http_headers", "late_order", _swap(text, at, at + 1))

    # Original practical_cases.cpp lines 167-185.
    term = "(x+2*3<=limit&&!(count==0||score>7))"
    text = "||".join([term] * _copies(scale, 0, len(term) + 2))
    emit("filter_expression", "valid", text)
    emit("filter_expression", "early_byte", "#" + text[1:])
    at = _find(text, "<=", len(text) // 2)
    emit("filter_expression", "middle_order", _swap(text, at, at + 1))
    at = _rfind(text, "<=")
    emit("filter_expression", "late_order", _swap(text, at, at + 1))

    # Original practical_cases.cpp lines 188-208.
    unit = "<b><a>ok</a>fine</b>"
    count = _copies(scale, 7, len(unit))
    text = "<a>" + unit * count + "</a>"
    emit("nested_tags", "valid", text)
    emit("nested_tags", "early_byte", "!" + text[1:])
    at = _find(text, "</a>", len(text) // 2)
    other = _find(text, "</b>", at)
    emit("nested_tags", "middle_order", _swap(text, at + 2, other + 2))
    at = 3 + (count - 1) * len(unit) + _find(unit, "</a>")
    other = _find(text, "</b>", at)
    emit("nested_tags", "late_order", _swap(text, at + 2, other + 2))

    # Original practical_cases.cpp lines 211-233.
    count = max(1, (scale - 8) // 20)
    a, b = "A123456\n", "B1234567890\n"
    text = "HDR\n" + a * count + b * count + "END\n"
    emit("fixed_record_batch", "valid", text)
    emit("fixed_record_batch", "early_byte", "!" + text[1:])
    at = 4 + (count - 1) * len(a)
    changed = text[:at] + b + a + text[at + len(a) + len(b):]
    emit("fixed_record_batch", "middle_order", changed)
    at = len(text) - 4 - len(b)
    emit("fixed_record_batch", "late_order", _swap(text, at, at + 1))
    at = len(text) - 4
    emit("fixed_record_batch", "length_plus_one", text[:at] + "0" + text[at:])

    # Original practical_cases.cpp lines 240-255, selected depth 1024 only.
    depth = 1024
    text = '[{"x":' * depth + '0' + '}]' * depth
    emit("json_deep", "valid", text, depth)
    at = _find(text, "}]")
    emit("json_deep", "middle_order", _swap(text, at, at + 1), depth)
    at = len(text) - 4
    emit("json_deep", "late_order", _swap(text, at, at + 1), depth)
    return result


def practical_cases() -> list[dict]:
    metadata = json.loads((HERE.parent / "data" / "practical-cases.metadata.json").read_text(encoding="utf-8"))
    subjects = _subjects()
    if len(metadata) != 28 or set(subjects) != {case["id"] for case in metadata}:
        raise ValueError("original practical case inventory changed")
    cases = []
    for original in metadata:
        case = dict(original)
        case["subject"] = subjects[case["id"]]
        for field in ("pattern", "subject"):
            raw = case[field].encode("latin-1")
            if hashlib.sha256(raw).hexdigest() != case[f"{field}_sha256"]:
                raise ValueError(f"{case['id']}: {field} differs from archived fixture")
        if len(case["subject"].encode("latin-1")) != case["bytes"]:
            raise ValueError(f"{case['id']}: byte length differs from archived fixture")
        cases.append(case)
    return cases


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-only", action="store_true", help="verify all 28 original hashes without emitting subjects")
    args = parser.parse_args()
    cases = practical_cases()
    if args.verify_only:
        print("Verified all 28 original pattern and subject hashes; no recognizers run.")
    else:
        print(json.dumps(cases, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
