"""Independent regex/list/event grammars and oracles from the historical suite."""
from __future__ import annotations
from .grammar_sources import pair_map

import argparse
import hashlib
import json
import struct
from pathlib import Path


CRANK_GRAMMARS = pair_map(["regex_syntax", "event_stream", "nested_comma_list"], "crank")

MARPA_GRAMMARS = pair_map(["regex_syntax", "event_stream", "nested_comma_list"], "marpa")

SCOPES = {
    "regex_syntax": "Nonempty basic regex syntax over lowercase ASCII literals, grouping, alternation, concatenation, and at most one * + ? postfix per atom. Both grammar sources retain binary association ambiguity. Boolean whole-input membership only; no regex execution or parse-tree obligation.",
    "event_stream": "Nonempty LF-terminated event stream: E[key=value;...] with one or more fields; key [a-z][a-z0-9_]*; values nested events, possibly empty comma-separated lists, unsigned nonempty decimal integers, or quoted [A-Za-z0-9 _;=/-]* text. No whitespace outside strings, escapes, schema, or field relationships.",
    "nested_comma_list": "Nonempty nested comma lists with identifiers [a-z][a-z0-9]*, left-recursive list production, no whitespace or empty items. Boolean whole-input membership only.",
}


class Invalid(ValueError):
    pass


class Cursor:
    def __init__(self, text: str):
        self.text, self.pos = text, 0

    def take(self, literal: str) -> bool:
        if self.text.startswith(literal, self.pos):
            self.pos += len(literal)
            return True
        return False

    def need(self, literal: str) -> None:
        if not self.take(literal):
            raise Invalid("expected " + repr(literal))

    def run(self, chars: str, minimum: int = 0) -> None:
        start = self.pos
        while self.pos < len(self.text) and self.text[self.pos] in chars:
            self.pos += 1
        if self.pos - start < minimum:
            raise Invalid("missing byte run")


LOWER = "abcdefghijklmnopqrstuvwxyz"
DIGITS = "0123456789"
TEXT_BYTES = "ABCDEFGHIJKLMNOPQRSTUVWXYZ" + LOWER + DIGITS + " _;=/-"


def _regex_syntax(text: str) -> bool:
    # Iterative syntax checker; it neither constructs nor chooses a parse tree.
    depth, need_atom, allow_postfix = 0, True, False
    for byte in text:
        if need_atom:
            if byte in LOWER:
                need_atom, allow_postfix = False, True
            elif byte == "(":
                depth += 1
                allow_postfix = False
            else:
                return False
        elif byte in LOWER:
            allow_postfix = True
        elif byte == "(":
            depth += 1
            need_atom, allow_postfix = True, False
        elif byte in "*+?" and allow_postfix:
            allow_postfix = False
        elif byte == "|":
            need_atom, allow_postfix = True, False
        elif byte == ")" and depth:
            depth -= 1
            allow_postfix = True
        else:
            return False
    return not need_atom and depth == 0


def _event_stream(text: str) -> bool:
    # Explicit stack makes the oracle independent of Python's recursion limit.
    p = Cursor(text)
    records = 0
    try:
        while p.pos < len(text):
            stack = ["event"]
            while stack:
                state = stack.pop()
                if state == "event":
                    p.need("E[")
                    stack.extend(("event_tail", "field"))
                elif state == "field":
                    p.run(LOWER, 1)
                    p.run(LOWER + DIGITS + "_")
                    p.need("=")
                    stack.append("value")
                elif state == "event_tail":
                    if p.take(";"):
                        stack.extend(("event_tail", "field"))
                    else:
                        p.need("]")
                elif state == "list_tail":
                    if p.take(","):
                        stack.extend(("list_tail", "value"))
                    else:
                        p.need("]")
                elif state == "value":
                    if p.text.startswith("E[", p.pos):
                        stack.append("event")
                    elif p.take("["):
                        if not p.take("]"):
                            stack.extend(("list_tail", "value"))
                    elif p.take('"'):
                        p.run(TEXT_BYTES)
                        p.need('"')
                    else:
                        p.run(DIGITS, 1)
                else:
                    raise AssertionError(state)
            p.need("\n")
            records += 1
        return records > 0
    except Invalid:
        return False


def _comma_list(text: str) -> bool:
    p = Cursor(text)
    depth, need_item = 0, True
    try:
        while p.pos < len(text):
            if need_item:
                if p.take("("):
                    depth += 1
                else:
                    p.run(LOWER, 1)
                    p.run(LOWER + DIGITS)
                    need_item = False
            elif p.take(","):
                need_item = True
            elif p.take(")") and depth:
                depth -= 1
            else:
                return False
        return not need_item and depth == 0
    except Invalid:
        return False


ORACLES = {
    "regex_syntax": _regex_syntax,
    "event_stream": _event_stream,
    "nested_comma_list": _comma_list,
}


def accepts(case_or_family: dict | str, subject: str | bytes) -> bool:
    family = case_or_family if isinstance(case_or_family, str) else case_or_family["family"]
    if isinstance(subject, bytes):
        subject = subject.decode("latin1")
    if any(ord(byte) > 127 for byte in subject):
        return False
    return ORACLES[family](subject)


def _row(identifier: str, family: str, subject: str, expected: bool, **extra) -> dict:
    if accepts(family, subject) != expected:
        raise AssertionError((identifier, expected))
    return dict(id=identifier, family=family, subject=subject, expected=expected,
                pattern=CRANK_GRAMMARS[family], crank_grammar=CRANK_GRAMMARS[family],
                marpa_grammar=MARPA_GRAMMARS[family], bytes=len(subject.encode("latin1")),
                scope=SCOPES[family], obligation="boolean_whole_input_recognition",
                oracle="independent_iterative_syntax:" + family, **extra)


def cases() -> list[dict]:
    result = []
    positive = "(" + "alpha" + "a" * 7 + "|b?)*c"
    for expected, subject in ((True, positive), (False, positive + ")")):
        result.append(_row(f"regex_syntax_8_{'accepted' if expected else 'rejected'}",
                           "regex_syntax", subject, expected, scale=8,
                           fresh_marker="alpha", fresh_role="literal_run",
                           source_fixture="2026-10-12 adversarial challenge scale8"))
    event_row = 'E[id=17;msg="route";child=E[id=2;msg="ok"];samples=[1,2,3]]\n'
    for scale in (8192, 131072):
        positive = event_row * max(1, scale // len(event_row))
        at = positive.rfind(";samples=[") + 8
        assert positive[at:at + 2] == "=["
        negative = positive[:at] + "[=" + positive[at + 2:]
        for expected, subject in ((True, positive), (False, negative)):
            result.append(_row(f"event_stream_{scale}_{'valid' if expected else 'late_order'}",
                               "event_stream", subject, expected, scale=scale,
                               target_bytes=scale, mutation_offset=None if expected else at,
                               fresh_marker="route", fresh_role="quoted_text",
                               source_fixture="historical event row repeated to target; late samples=[ -> samples[="))
    body = ["(alpha,b)" if i % 4 == 0 or i == 7 else "alpha" for i in range(8)]
    positive = "(" + ",".join(body) + ")"
    for expected, subject in ((True, positive), (False, positive + ")")):
        result.append(_row(f"nested_comma_list_8_{'accepted' if expected else 'rejected'}",
                           "nested_comma_list", subject, expected, scale=8,
                           fresh_marker="alpha", fresh_role="identifier",
                           source_fixture="2026-10-12 adversarial left comma-list scale8"))
    assert len(result) == 8
    return result


def variants(case: dict, n: int = 16):
    """Original subject followed by distinct fixed-length fresh substitutions.

    This is fixture generation, not result-oracle caching. Engine verdicts are
    never consulted. Callers must allocate and check these outside warm timers.
    The variants change content, not allocation alone; use each once per batch.
    """
    if not isinstance(n, int) or not 0 <= n <= 26 ** 4 + 1:
        raise ValueError("n must be between 0 and 26**4 + 1")
    marker = case["fresh_marker"]
    if marker not in case["subject"]:
        raise ValueError("missing fresh-input marker")
    for index in range(n):
        if index == 0:
            subject = case["subject"]
        else:
            value, digits = index - 1, []
            for _ in range(4):
                digits.append(chr(ord("a") + value % 26))
                value //= 26
            replacement = "v" + "".join(reversed(digits))
            subject = case["subject"].replace(marker, replacement)
        assert len(subject) == len(case["subject"])
        assert accepts(case, subject) == case["expected"], case["id"]
        yield subject


def correctness_cases() -> list[dict]:
    edges = {
        "regex_syntax": {
            True: ["a", "a?", "(a|b)*", "abc", "a|b|c", "a|bc", "a*b?", "((a)*)?", "a(b|c)d"],
            False: ["", "()", "(a", "a)", "|a", "a|", "a||b", "a**", "a*?", "a??", "a++", "a b", "A", "1", "a\n", "a\x00", "a\x80"],
        },
        "event_stream": {
            True: ['E[a=0]\n', 'E[a=001]\n', 'E[a=[]]\n', 'E[a=""]\n', 'E[a0_b=1;b=2]\n', 'E[a=E[b=2]]\n', 'E[a=[1,E[b=2],[],"x"]]\n', 'E[a="a A0_;=/-"]\n', 'E[a=1]\nE[b=2]\n'],
            False: ["", "\n", "E[]\n", "E[a=]\n", "E[a=1]", "E[a=1]\r\n", "E[a=1]\n\n", " E[a=1]\n", "E[a=1 ]\n", "E[a=1;]\n", "E[a=[1,]]\n", "E[a=[,1]]\n", "E[a=E[]]\n", "E[a=E[b=2]\n]\n", "E[A=1]\n", "E[1a=1]\n", "E[a=-1]\n", "E[a=1.0]\n", 'E[a="a:b"]\n', 'E[a="a\\b"]\n', 'E[a="a\tb"]\n', 'E[a="a\nb"]\n', 'E[a="\x80"]\n', "E[a=1]\n\x00"],
        },
        "nested_comma_list": {
            True: ["a", "a0", "ab12", "a,b", "(a)", "((a,b),c)", "((a))", "a,(b,c),d"],
            False: ["", "()", "a,", ",a", "a,,b", "(a,)", "(,a)", "(a", "a)", "(a)b", "a(b)", "a_", "A", "0a", "a b", "a\n", "a\xff"],
        },
    }
    result = []
    for family, verdicts in edges.items():
        for expected, subjects in verdicts.items():
            for index, subject in enumerate(subjects):
                result.append(_row(f"edge_{family}_{int(expected)}_{index:02d}",
                                   family, subject, expected, suite="correctness_edge"))
    # Shared alphabets can represent arbitrarily nested inputs without recursive
    # Python call depth imposing an undocumented restriction on the oracle.
    result.append(_row("edge_regex_depth64", "regex_syntax", "(" * 64 + "a" + ")" * 64, True, suite="correctness_edge"))
    result.append(_row("edge_list_depth64", "nested_comma_list", "(" * 64 + "a" + ")" * 64, True, suite="correctness_edge"))
    result.append(_row("edge_event_depth64", "event_stream", "E[a=" * 64 + "0" + "]" * 64 + "\n", True, suite="correctness_edge"))
    return result



