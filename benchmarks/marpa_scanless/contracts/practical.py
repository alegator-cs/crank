"""Public synthetic practical fixtures; historical grammar bytes and oracles."""
from __future__ import annotations
from .grammar_sources import pair_map

import json
import hashlib
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
from . import base as known

JSON_CRANK = pair_map(["restricted_json"], "crank")["restricted_json"]

JSON_MARPA = pair_map(["restricted_json"], "marpa")["restricted_json"]

JSON_SCOPE = (
    "JSON objects, arrays, strings, decimal numbers, true/false/null, and JSON "
    "whitespace. Unescaped strings are restricted to ASCII [A-Za-z0-9 _;=:/.-]; "
    "all JSON short escapes and exactly four-hex \\u escapes are admitted. "
    "No trailing commas, comments, NaN/Infinity, schema, duplicate-key check, "
    "numeric conversion obligation, or semantic/parse-tree output. Boolean "
    "whole-input membership only. This is a declared subset of JSON."
)

CRANK_GRAMMARS = {**known.CRANK_GRAMMARS, "restricted_json": JSON_CRANK}
MARPA_GRAMMARS = {**known.MARPA_GRAMMARS, "restricted_json": JSON_MARPA}
SCOPES = {**known.SCOPES, "restricted_json": JSON_SCOPE}
FAMILY_ORDER = ["regex_syntax", "nested_comma_list", "event_stream", "restricted_json"]


def json_accepts(subject: str) -> bool:
    """Independent stdlib parser plus a separate restricted raw-string scanner."""
    if not subject.isascii():
        return False
    plain = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _;=:/.-")
    hexes = set("0123456789abcdefABCDEF")
    at, in_string = 0, False
    while at < len(subject):
        c = subject[at]
        if not in_string:
            if c == '"':
                in_string = True
            at += 1
        elif c == '"':
            in_string = False
            at += 1
        elif c == "\\":
            at += 1
            if at >= len(subject):
                return False
            esc = subject[at]
            if esc in '"\\/bfnrt':
                at += 1
            elif esc == "u" and at + 4 < len(subject) and all(x in hexes for x in subject[at + 1:at + 5]):
                at += 5
            else:
                return False
        elif c in plain:
            at += 1
        else:
            return False
    if in_string:
        return False
    try:
        def reject_non_json_constant(value):
            raise ValueError(value)
        json.loads(subject, parse_constant=reject_non_json_constant)
        return True
    except (ValueError, RecursionError):
        return False


def accepts(family: str, subject: str) -> bool:
    return json_accepts(subject) if family == "restricted_json" else known.accepts(family, subject)


def restricted_serialization(value) -> str:
    """Preserve the JSON value while escaping raw string characters outside the subset."""
    raw = json.dumps(value, separators=(",", ":"), ensure_ascii=True)
    plain = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _;=:/.-")
    out, at, inside = [], 0, False
    while at < len(raw):
        ch = raw[at]
        if ch == '"':
            inside = not inside
            out.append(ch)
            at += 1
        elif inside and ch == "\\":
            n = 6 if raw[at + 1] == "u" else 2
            out.append(raw[at:at + n])
            at += n
        elif inside and ch not in plain:
            out.append("\\u%04x" % ord(ch))
            at += 1
        else:
            out.append(ch)
            at += 1
    result = "".join(out)
    assert json.loads(result) == value
    assert json_accepts(result)
    return result


def synthetic_json() -> tuple[str, dict]:
    value = {"benchmark_marker": "alpha", "enabled": True,
             "packages": [{"name": "package%02d" % i, "version": "1.0",
                           "dependencies": ["common", "utility"],
                           "settings": {"attempts": i + 1, "cache": False}}
                          for i in range(32)]}
    return restricted_serialization(value), {"kind": "synthetic_public_fixture"}


def entries() -> list[dict]:
    """Four families x two preset sizes x accepted/malformed; selected before timing."""
    positive = {
        "regex_syntax": {
            "small": "(alpha|beta)*gamma?",
            "large": "((alpha|beta)+gamma?)?(delta|epsilon)*(alpha|gamma)?beta+(theta|lambda)*omega?",
        },
        "nested_comma_list": {
            "small": "(alpha,beta,(gamma,delta),epsilon,theta,(eta,kappa))",
            "large": "(" + ",".join(
                f"(alpha{i},beta{i},(gamma{i},delta{i}),epsilon{i})" for i in range(72)
            ) + ")",
        },
        "event_stream": {},
        "restricted_json": {},
    }
    event_row = 'E[id=17;msg="route";child=E[id=2;msg="ok"];samples=[1,2,3]]\n'
    positive["event_stream"] = {"small": event_row * 2, "large": event_row * 128}
    positive["restricted_json"]["small"] = (
        '{"name":"alpha","enabled":true,"items":[1,2,{"code":"ok","value":-3.5e+2}],"empty":null}'
    )
    positive["restricted_json"]["large"], receipt_binding = synthetic_json()
    descriptions = {
        "regex_syntax": "Handwritten regex syntax over supported literal/group/alternation/postfix syntax; lowercase words model token names. The input regex is recognized, never executed.",
        "nested_comma_list": "Generated nested identifier lists resembling configuration/function argument lists; 1 small fixed list or 72 nested groups.",
        "event_stream": "Synthetic event log rows with numeric fields, quoted messages, a nested child event and numeric samples; 2 or 128 complete LF-terminated rows.",
        "restricted_json": "Small synthetic mixed JSON configuration object; larger synthetic configuration with nested arrays and objects.",
    }
    rows = []
    for family in FAMILY_ORDER:
        for size in ("small", "large"):
            good = positive[family][size]
            marker = "route" if family == "event_stream" else "alpha"
            if family in ("regex_syntax", "nested_comma_list"):
                bad, offset = good + ")", len(good)
                mutation = "Append one unmatched closing parenthesis at physical EOF."
            elif family == "event_stream":
                offset = good.rfind(";samples=[") + len(";samples")
                assert good[offset:offset + 2] == "=["
                bad = good[:offset] + "[=" + good[offset + 2:]
                mutation = "Swap the final samples=[ delimiter to samples[=; all earlier rows remain intact."
            else:
                assert good[-1] == "}"
                offset = len(good) - 1
                bad = good[:-1] + "]"
                mutation = "Replace the root object's last closing brace with a closing square bracket."
            for expected, subject in ((True, good), (False, bad)):
                assert accepts(family, subject) == expected, (family, size, expected)
                derived = family == "restricted_json" and size == "large"
                rows.append({
                    "id": f"{family}_{size}_{'accepted' if expected else 'malformed'}",
                    "family": family, "size": size, "subject": subject, "expected": expected,
                    "bytes": len(subject.encode("ascii")), "fresh_marker": marker,
                    "scope": SCOPES[family], "obligation": "boolean_whole_input_recognition",
                    "provenance": "synthetic_public_fixture",
                    "source_data_binding": receipt_binding if derived else None,
                    "construction": descriptions[family],
                    "mutation": None if expected else mutation,
                    "mutation_offset": None if expected else offset,
                    "planned_weight": 1 / 32,
                })
    assert len(rows) == 16
    return rows


def fresh_subject(entry: dict, index: int) -> str:
    """Distinct same-byte-length subjects, with a content change preserving structure."""
    if not 0 <= index < 26 ** 4 + 1:
        raise ValueError(index)
    if index == 0:
        subject = entry["subject"]
    else:
        value, suffix = index - 1, []
        for _ in range(4):
            suffix.append(chr(ord("a") + value % 26))
            value //= 26
        replacement = "v" + "".join(reversed(suffix))
        subject = entry["subject"].replace(entry["fresh_marker"], replacement)
    assert len(subject) == entry["bytes"]
    assert accepts(entry["family"], subject) == entry["expected"]
    return subject


def json_boundary_cases() -> list[tuple[str, bool]]:
    accepted = [
        '{}', '[]', 'null', 'true', 'false', '0', '-0', '123', '-12.25e+3',
        ' "" \n', '{ "alpha" : [1, true, false, null, {}, []] }',
        '"alpha\\n\\t\\r\\b\\f\\/\\\\\\\"\\u0041"',
    ]
    malformed = [
        '', '{]', '[}', '[1,]', '{"alpha":1,}', '{"alpha" 1}', '01', '-01',
        '1.', '.1', '+1', '1e', '1e+', 'NaN', 'Infinity', 'true false',
        '"a:b!"', '"a\nb"', '"a\\x20"', '"a\\u123"', '"a\\uZZZZ"',
    ]
    rows = [(s, True) for s in accepted] + [(s, False) for s in malformed]
    assert all(json_accepts(s) == expected for s, expected in rows)
    return rows
