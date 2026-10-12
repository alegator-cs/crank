"""Canonical final frozen grammar bytes, bound to historical manifest hashes."""
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROVENANCE = ROOT / "GRAMMAR_SOURCES.json"


def validate_marpa(text):
    """Static runner-contract lint; Marpa still parses the DSL at runtime."""
    if not text.isascii():
        raise ValueError("Marpa grammar must be ASCII")
    if not re.search(r"^\s*:start\s*::=\s*START\s*$", text, re.M):
        raise ValueError("missing START declaration")
    if not re.search(r"^\s*event\s+ROOT_DONE\s*=\s*completed\s+START\s*$", text, re.M):
        raise ValueError("missing physical-EOF completion event")
    if re.search(r"^\s*:discard\b", text, re.M):
        raise ValueError("discard changes byte-position contract")
    wrappers = 0
    for line in text.splitlines():
        if re.match(r"^\s*#", line):
            continue
        if re.match(r"^\s*START\s*::=\s*([A-Za-z][A-Za-z0-9_]*)\s*$", line):
            wrappers += 1
            continue
        match = re.match(r"^\s*[^:#].*?::=(.*)$", line)
        if match and re.search(r"\bSTART\b", match.group(1)):
            raise ValueError("START appears on a G1 RHS")
        if re.search(r"::=\s*\|", line):
            raise ValueError("nullable Marpa rule must be declared separately")
    if wrappers != 1:
        raise ValueError("START needs exactly one single-symbol wrapper rule")


def pair_map(families, kind):
    bindings = json.loads(PROVENANCE.read_text())["grammars"]
    result = {}
    for family in families:
        binding = bindings[family][kind]
        data = (ROOT / binding["path"]).read_bytes()
        if len(data) != binding["bytes"] or hashlib.sha256(data).hexdigest() != binding["sha256"]:
            raise ValueError("final frozen grammar hash mismatch: " + family + "/" + kind)
        text = data.decode("ascii")
        if kind == "marpa":
            validate_marpa(text)
        result[family] = text
    return result
