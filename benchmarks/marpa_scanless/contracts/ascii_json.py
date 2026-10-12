"""Broad ASCII JSON contract from the wide phase; no local data is loaded."""
import json
from .grammar_sources import pair_map

SCOPE = "ASCII JSON syntax with all printable raw string bytes except quote/backslash; standard JSON escapes; no scalar, duplicate-key, schema or semantic-value obligation."
CRANK = pair_map(["json_ascii"], "crank")["json_ascii"]
MARPA = pair_map(["json_ascii"], "marpa")["json_ascii"]

def reject_constant(value):
    raise ValueError(value)

def accepts(subject):
    if not subject.isascii() or "\x7f" in subject:
        return False
    try:
        json.loads(subject, parse_constant=reject_constant)
        return True
    except (ValueError, RecursionError):
        return False
