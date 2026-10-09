#!/usr/bin/env python3
"""Independent language oracles for the Marpa comparison inventory.

This file never imports Crank, Marpa, their ASTs, or their verdicts.  Each
predicate checks the subject against the documented fixture language.  The
``expected`` field is compared with the predicate's result only afterward.
These are deliberately narrow benchmark languages, not production validators.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any, Callable


class Invalid(ValueError):
    """A subject does not belong to a fixture language."""


def _ascii(text: str) -> bool:
    # This fixture's unescaped string bytes stop at '~', excluding DEL too.
    return all(ord(ch) < 127 for ch in text)


def _json_constant(value: str) -> None:
    raise Invalid("JSON's grammar has no " + value + " constant")


def json_ascii(subject: str, case: dict[str, Any]) -> bool:
    if not _ascii(subject):
        return False
    if sys.getrecursionlimit() < 20000:
        sys.setrecursionlimit(20000)
    try:
        json.loads(subject, parse_constant=_json_constant)
        return True
    except ValueError:
        return False


def http_subset(subject: str, case: dict[str, Any]) -> bool:
    # Explicit CRLF and exactly one terminating blank line; no message body.
    if not subject.endswith("\r\n\r\n"):
        return False
    lines = subject.split("\r\n")
    if len(lines) < 3 or lines[-2:] != ["", ""]:
        return False
    request = lines[0].split(" ")
    if len(request) != 3:
        return False
    method, target, version = request
    if method not in ("GET", "POST") or version != "HTTP/1.1":
        return False
    if not target.startswith("/"):
        return False
    if any(ch not in "abcdefghijklmnopqrstuvwxyz0123456789/_?=&-"
           for ch in target[1:]):
        return False
    for header in lines[1:-2]:
        if ": " not in header:
            return False
        key, value = header.split(": ", 1)
        if not key or not key[0].isascii() or not key[0].isalpha():
            return False
        if any(not (ch.isascii() and (ch.isalpha() or ch == "-"))
               for ch in key[1:]):
            return False
        if any(not 32 <= ord(ch) <= 126 for ch in value):
            return False
    return True


def tag_stack(subject: str, case: dict[str, Any]) -> bool:
    if not subject.startswith("<a>"):
        return False
    stack: list[str] = []
    pos = 0
    while pos < len(subject):
        if subject[pos] == "<":
            found = False
            for name in ("a", "b"):
                opening, closing = "<" + name + ">", "</" + name + ">"
                if subject.startswith(opening, pos):
                    stack.append(name)
                    pos += len(opening)
                    found = True
                    break
                if subject.startswith(closing, pos):
                    if not stack or stack.pop() != name:
                        return False
                    pos += len(closing)
                    found = True
                    if not stack:
                        return pos == len(subject)
                    break
            if not found:
                return False
        else:
            if not stack or subject[pos] not in "abcdefghijklmnopqrstuvwxyz ":
                return False
            pos += 1
    return False


def fixed_records(subject: str, case: dict[str, Any]) -> bool:
    lines = subject.split("\n")
    if len(lines) < 3 or lines[0] != "HDR" or lines[-2:] != ["END", ""]:
        return False
    in_b = False
    for line in lines[1:-2]:
        if not line:
            return False
        if line[0] == "A":
            if in_b or len(line) != 7:
                return False
        elif line[0] == "B":
            in_b = True
            if len(line) != 11:
                return False
        else:
            return False
        if any(ch not in "0123456789" for ch in line[1:]):
            return False
    return True


class Cursor:
    def __init__(self, text: str):
        self.text = text
        self.pos = 0

    def take(self, token: str) -> bool:
        if self.text.startswith(token, self.pos):
            self.pos += len(token)
            return True
        return False

    def need(self, token: str) -> None:
        if not self.take(token):
            raise Invalid("expected " + repr(token))

    def run(self, chars: str, minimum: int = 0) -> str:
        start = self.pos
        while self.pos < len(self.text) and self.text[self.pos] in chars:
            self.pos += 1
        if self.pos - start < minimum:
            raise Invalid("missing lexical run")
        return self.text[start:self.pos]

    def done(self) -> bool:
        return self.pos == len(self.text)


def event_stream(subject: str, case: dict[str, Any]) -> bool:
    c = Cursor(subject)

    def field() -> None:
        c.run("abcdefghijklmnopqrstuvwxyz", 1)
        c.run("abcdefghijklmnopqrstuvwxyz0123456789_")
        c.need("=")
        value()

    def event() -> None:
        c.need("E[")
        field()
        while c.take(";"):
            field()
        c.need("]")

    def value() -> None:
        if c.text.startswith("E[", c.pos):
            event()
        elif c.take("["):
            if not c.take("]"):
                value()
                while c.take(","):
                    value()
                c.need("]")
        elif c.take('"'):
            c.run("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _;=/-")
            c.need('"')
        else:
            c.run("0123456789", 1)

    try:
        event()
        c.need("\n")
        while not c.done():
            event()
            c.need("\n")
        return True
    except Invalid:
        return False


def filter_expression(subject: str, case: dict[str, Any]) -> bool:
    c = Cursor(subject)

    def factor() -> None:
        if c.take("("):
            arithmetic()
            c.need(")")
        elif c.pos < len(c.text) and c.text[c.pos] in "0123456789":
            c.run("0123456789", 1)
        else:
            c.run("abcdefghijklmnopqrstuvwxyz", 1)
            c.run("abcdefghijklmnopqrstuvwxyz0123456789_")

    def term() -> None:
        factor()
        while c.take("*") or c.take("/"):
            factor()

    def arithmetic() -> None:
        term()
        while c.take("+") or c.take("-"):
            term()

    def predicate() -> None:
        start = c.pos
        try:
            arithmetic()
            if not any(c.take(op) for op in ("==", "!=", "<=", ">=", "<", ">")):
                raise Invalid("missing comparison")
            arithmetic()
            return
        except Invalid:
            c.pos = start
        c.take("!")
        c.need("(")
        expression()
        c.need(")")

    def conjunction() -> None:
        predicate()
        while c.take("&&"):
            predicate()

    def expression() -> None:
        conjunction()
        while c.take("||"):
            conjunction()

    try:
        expression()
        return c.done()
    except Invalid:
        return False


def nested_equal(subject: str, case: dict[str, Any]) -> bool:
    return len(subject) % 2 == 0 and subject == (
        "a" * (len(subject) // 2) + "b" * (len(subject) // 2))


def dyck(subject: str, case: dict[str, Any]) -> bool:
    pairs = {"(": ")"} if case["family"] == "dyck1_flat" else {"(": ")", "[": "]"}
    stack: list[str] = []
    for ch in subject:
        if ch in pairs:
            stack.append(pairs[ch])
        elif not stack or stack.pop() != ch:
            return False
    return not stack


def unary_a(subject: str, case: dict[str, Any]) -> bool:
    return all(ch == "a" for ch in subject)


def digit_expression(subject: str, case: dict[str, Any]) -> bool:
    operand = True
    depth = 0
    for ch in subject:
        if operand:
            if ch == "(":
                depth += 1
            elif ch in "0123456789":
                operand = False
            else:
                return False
        elif ch in "+*":
            operand = True
        elif ch == ")" and depth:
            depth -= 1
        else:
            return False
    return not operand and depth == 0


def catalan(subject: str, case: dict[str, Any]) -> bool:
    return len(subject) >= 3 and subject[-1] == "b" and all(ch == "a" for ch in subject[:-1])


def nullable_cycle(subject: str, case: dict[str, Any]) -> bool:
    return subject in ("", "a")


def count_gcd(subject: str, case: dict[str, Any]) -> bool:
    if case["pattern"] != "(?:a{6})*(?:a{10})*":
        raise ValueError("count_gcd fixture grammar changed; update the independent contract")
    return unary_a(subject, case) and any(
        (len(subject) - 10 * y) % 6 == 0 for y in range(len(subject) // 10 + 1))


def count_lattice(subject: str, case: dict[str, Any]) -> bool:
    if case["pattern"] != "(?:a{127})*(?:a{131})*":
        raise ValueError("count_lattice fixture grammar changed; update the independent contract")
    # Exhaustive nonnegative allocation enumeration is independent of the
    # engine's modular-inverse presolve and its residue abstractions.
    return unary_a(subject, case) and any(
        (len(subject) - 127 * x) % 131 == 0
        for x in range(len(subject) // 127 + 1))


def count_bounded(subject: str, case: dict[str, Any]) -> bool:
    grammar = re.fullmatch(
        r"\(\?:a\{(\d+)\}\)\{(\d+),(\d+)\}\(\?:a\{(\d+)\}\)\{(\d+),(\d+)\}",
        case["pattern"])
    if not grammar:
        raise ValueError("count_bounded fixture grammar changed; update the independent contract")
    a, lo_x, hi_x, b, lo_y, hi_y = map(int, grammar.groups())
    return unary_a(subject, case) and any(
        a * x + b * y == len(subject)
        for x in range(lo_x, hi_x + 1) for y in range(lo_y, hi_y + 1))


def independent_branches(subject: str, case: dict[str, Any]) -> bool:
    # Read only the literal markers from a checked fixture-pattern form.  This
    # is not a general Crank-pattern interpreter and calls no engine parser.
    arm = re.compile(r"\(\?<(?P<name>R\d+)>a\(\?&(?P=name)\)b\|c(?P<marker>[a-z])d\)")
    markers: set[str] = set()
    pattern = case["pattern"]
    pos = 0
    while pos < len(pattern):
        match = arm.match(pattern, pos)
        if match is None:
            raise ValueError("independent_branches fixture grammar changed")
        markers.add(match["marker"])
        pos = match.end()
        if pos < len(pattern):
            if pattern[pos] != "|" or pos + 1 == len(pattern):
                raise ValueError("independent_branches has malformed arm separator")
            pos += 1
    word = re.fullmatch(r"(a*)c([a-z])d(b*)", subject)
    return word is not None and len(word[1]) == len(word[3]) and word[2] in markers


def shared_prefix(subject: str, case: dict[str, Any]) -> bool:
    word = re.fullmatch(r"(a*)(b*)cc([A-H])dd", subject)
    return word is not None and len(word[1]) == len(word[2])


ORACLES: dict[str, Callable[[str, dict[str, Any]], bool]] = {
    "python_json_loads_ascii": json_ascii,
    "http_subset_lines": http_subset,
    "tag_stack_two_names": tag_stack,
    "fixed_record_lengths_and_fields": fixed_records,
    "event_recursive_descent": event_stream,
    "arithmetic_recursive_descent": filter_expression,
}

CORE_ORACLES: dict[str, Callable[[str, dict[str, Any]], bool]] = {
    "nested_equal": nested_equal,
    "dyck1_flat": dyck,
    "dyck2_nested": dyck,
    "left_recursive": unary_a,
    "right_recursive": unary_a,
    "ambiguous_expression": digit_expression,
    "catalan": catalan,
    "catalan_budget": catalan,
    "nullable_cycle": nullable_cycle,
    "count_gcd": count_gcd,
    "count_lattice": count_lattice,
    "count_bounded": count_bounded,
    "independent_branches": independent_branches,
    "shared_prefix": shared_prefix,
    "mixed_prefix": shared_prefix,
}


def check_core_contract(case: dict[str, Any]) -> None:
    """Fail closed if a named fixture changes its documented language."""
    patterns = {
        "nested_equal": r"(?<S>a(?&S)b|)",
        "dyck1_flat": r"(?<S>[(](?&S)[)](?&S)|)",
        "dyck2_nested": r"(?<S>[(](?&S)[)](?&S)|\[(?&S)\](?&S)|)",
        "left_recursive": r"(?<S>(?&S)a|)",
        "right_recursive": r"(?<S>a(?&S)|)",
        "ambiguous_expression": r"(?<E>(?&E)[+](?&E)|(?&E)[*](?&E)|[(](?&E)[)]|[0-9])",
        "catalan": r"a(?<R>(?&R)(?&R)|a)b",
        "catalan_budget": r"a(?<R>(?&R)(?&R)|a)b",
        "nullable_cycle": r"(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)",
    }
    family = case["family"]
    if family in patterns and case["pattern"] != patterns[family]:
        raise ValueError(f"{family}: fixture grammar changed; update the independent contract")
    if family in ("shared_prefix", "mixed_prefix"):
        arms = []
        for i, marker in enumerate("ABCDEFGH"):
            name = "R" + str(i // 2 if family == "mixed_prefix" else 0)
            define = i % 2 == 0 if family == "mixed_prefix" else i == 0
            prefix = "(?<" + name + ">a(?&" + name + ")b|)" if define else "(?&" + name + ")"
            arms.append(prefix + "cc" + marker + "dd")
        if case["pattern"] != "|".join(arms):
            raise ValueError(f"{family}: fixture grammar changed; update the independent contract")


def validate_cases(cases: list[dict[str, Any]]) -> dict[str, Any]:
    if not isinstance(cases, list) or not cases:
        raise ValueError("inventory must be a nonempty JSON array")
    seen: set[str] = set()
    rows: list[dict[str, Any]] = []
    domains: Counter[str] = Counter()
    oracles: Counter[str] = Counter()
    verdicts: Counter[str] = Counter()
    for case in cases:
        for field in ("id", "family", "domain", "variant", "pattern", "subject",
                      "expected", "scale", "mutation_offset", "practical", "oracle",
                      "scope", "allow_limit", "prune", "max_states", "max_steps"):
            if field not in case:
                raise ValueError(f"missing {field!r} in inventory item")
        name = case["id"]
        if not isinstance(name, str) or not name or name in seen:
            raise ValueError(f"invalid or duplicate case id: {name!r}")
        seen.add(name)
        if not isinstance(case["subject"], str) or not isinstance(case["pattern"], str):
            raise ValueError(f"{name}: pattern and subject must be strings")
        if type(case["expected"]) is not bool or type(case["practical"]) is not bool:
            raise ValueError(f"{name}: expected and practical must be booleans")
        if case["practical"]:
            oracle = case["oracle"]
            predicate = ORACLES.get(oracle)
        else:
            check_core_contract(case)
            oracle = "core:" + case["family"]
            predicate = CORE_ORACLES.get(case["family"])
        if predicate is None:
            raise ValueError(f"{name}: no independent oracle for {oracle!r}")
        actual = predicate(case["subject"], case)
        if type(actual) is not bool:
            raise TypeError(f"{name}: oracle did not return bool")
        if actual != case["expected"]:
            raise AssertionError(f"{name}: independent {oracle} says {actual}, "
                                 f"inventory expected {case['expected']}")
        domains[case["domain"]] += 1
        oracles[oracle] += 1
        verdicts["accept" if actual else "reject"] += 1
        # Latin-1 preserves the byte-valued strings emitted by the C++ inventory.
        try:
            raw = case["subject"].encode("latin-1")
        except UnicodeEncodeError as exc:
            raise ValueError(f"{name}: subject contains a non-byte character") from exc
        rows.append({"id": name, "oracle": oracle, "valid": actual,
                     "fixture_certificate": case["oracle"],
                     "practical": case["practical"], "bytes": len(raw),
                     "subject_sha256": hashlib.sha256(raw).hexdigest()})
    canonical = json.dumps(cases, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=True).encode("ascii")
    return {"passed": True, "case_count": len(rows),
            "status": "pass", "method": "independent language predicates",
            "checked": len(rows), "practical_checked": sum(x["practical"] for x in rows),
            "counts_by_domain": dict(sorted(domains.items())),
            "counts_by_oracle": dict(sorted(oracles.items())),
            "counts_by_verdict": dict(sorted(verdicts.items())),
            "inventory_canonical_sha256": hashlib.sha256(canonical).hexdigest(),
            "cases": rows}


def self_check() -> dict[str, Any]:
    checks = [
        ("python_json_loads_ascii", ' {"a":[true,null,-2.3e+2,"\\u1234"]} ', True),
        ("python_json_loads_ascii", "[01]", False),
        ("python_json_loads_ascii", "NaN", False),
        ("python_json_loads_ascii", '"é"', False),
        ("python_json_loads_ascii", '"\x7f"', False),
        ("http_subset_lines", "GET / HTTP/1.1\r\n\r\n", True),
        ("http_subset_lines", "POST /a?b=c HTTP/1.1\r\nX-Test: z\r\n\r\n", True),
        ("http_subset_lines", "GET / HTTP/1.1\n\n", False),
        ("http_subset_lines", "GET / HTTP/1.1\r\nX: z\r\n\r\nbody", False),
        ("tag_stack_two_names", "<a><b>x</b> y<a></a></a>", True),
        ("tag_stack_two_names", "<a><b></a></b>", False),
        ("tag_stack_two_names", "<b></b>", False),
        ("fixed_record_lengths_and_fields", "HDR\nEND\n", True),
        ("fixed_record_lengths_and_fields", "HDR\nA123456\nB1234567890\nEND\n", True),
        ("fixed_record_lengths_and_fields", "HDR\nB1234567890\nA123456\nEND\n", False),
        ("fixed_record_lengths_and_fields", "HDR\nA12345\nEND\n", False),
        ("event_recursive_descent", 'E[k=E[n=01];v=[[],2,"a;b=c"]]\nE[z=0]\n', True),
        ("event_recursive_descent", "E[k=[]]\n", True),
        ("event_recursive_descent", "E[]\n", False),
        ("event_recursive_descent", "E[k=1;]\n", False),
        ("event_recursive_descent", "E[k=1]", False),
        ("arithmetic_recursive_descent", "(x+2*3<=limit&&!(count==0||score>7))", True),
        ("arithmetic_recursive_descent", "(x+2)*3<y", True),
        ("arithmetic_recursive_descent", "!(x==0)", True),
        ("arithmetic_recursive_descent", "!x==0", False),
        ("arithmetic_recursive_descent", "!!(x==0)", False),
        ("arithmetic_recursive_descent", "x=<2", False),
        ("arithmetic_recursive_descent", "x==2&&", False),
        ("arithmetic_recursive_descent", "x", False),
    ]
    for oracle, subject, expected in checks:
        actual = ORACLES[oracle](subject, {})
        if actual != expected:
            raise AssertionError((oracle, subject, actual, expected))
    core_checks = [
        ("nested_equal", "aaabbb", True), ("nested_equal", "aababb", False),
        ("dyck1_flat", "()(())", True), ("dyck1_flat", "())(", False),
        ("dyck2_nested", "([])[]", True), ("dyck2_nested", "([)]", False),
        ("ambiguous_expression", "(1+2)*3", True),
        ("ambiguous_expression", "1++2", False),
        ("ambiguous_expression", "12", False),
        ("catalan", "aab", True), ("catalan", "ab", False),
        ("nullable_cycle", "", True), ("nullable_cycle", "aa", False),
        ("shared_prefix", "aabbccHdd", True),
        ("shared_prefix", "aabbccZdd", False),
    ]
    for family, subject, expected in core_checks:
        actual = CORE_ORACLES[family](subject, {"family": family})
        if actual != expected:
            raise AssertionError((family, subject, actual, expected))
    parameter_checks = [
        ("count_gcd", "a" * 64, "(?:a{6})*(?:a{10})*", True),
        ("count_gcd", "a" * 65, "(?:a{6})*(?:a{10})*", False),
        ("count_lattice", "a" * 16379, "(?:a{127})*(?:a{131})*", False),
        ("count_lattice", "a" * 16380, "(?:a{127})*(?:a{131})*", True),
        ("count_bounded", "a" * 64, "(?:a{13}){2,5}(?:a{19}){1,4}", True),
        ("count_bounded", "a" * 65, "(?:a{13}){2,5}(?:a{19}){1,4}", False),
        ("independent_branches", "aacqdbb", "(?<R0>a(?&R0)b|cqd)", True),
        ("independent_branches", "aacqdbb", "(?<R0>a(?&R0)b|chd)", False),
    ]
    for family, subject, pattern, expected in parameter_checks:
        actual = CORE_ORACLES[family](subject, {"family": family, "pattern": pattern})
        if actual != expected:
            raise AssertionError((family, subject, actual, expected))
    return {"status": "pass", "self_checks": len(checks) + len(core_checks) + len(parameter_checks)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cases", nargs="?", type=Path)
    parser.add_argument("output", nargs="?", type=Path,
                        default=Path("oracle_validation.json"))
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    # Fixture nesting is finite and bounded.  This changes the oracle's host
    # stack limit, never the definition or verdict of a benchmark language.
    sys.setrecursionlimit(max(sys.getrecursionlimit(), 10000))
    if args.self_check:
        print(json.dumps(self_check(), sort_keys=True))
    if args.cases:
        inventory_bytes = args.cases.read_bytes()
        inventory = json.loads(inventory_bytes)
        summary = validate_cases(inventory)
        summary["cases_sha256"] = hashlib.sha256(inventory_bytes).hexdigest()
        args.output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
        print(json.dumps({k: v for k, v in summary.items() if k != "cases"},
                         sort_keys=True))
    elif not args.self_check:
        parser.error("provide cases.json or --self-check")


if __name__ == "__main__":
    main()
