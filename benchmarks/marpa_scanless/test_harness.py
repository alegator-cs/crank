"""Contract and protocol checks; no compiler, engine, or timing jobs."""
import importlib.util
import hashlib
import json
import struct
import unittest
from pathlib import Path

import generate
from contracts import additional, ascii_json, base, practical
from contracts.grammar_sources import validate_marpa


class Contracts(unittest.TestCase):
    def test_final_frozen_grammar_hashes_and_wrapper_contract(self):
        bindings = json.loads((Path(__file__).parent / "GRAMMAR_SOURCES.json").read_text())["grammars"]
        self.assertEqual(len(bindings), 9)
        for family, pair in bindings.items():
            for kind, expected in pair.items():
                text = (generate.GRAMMARS if kind == "crank" else generate.MARPA)[family]
                self.assertEqual(hashlib.sha256(text.encode("ascii")).hexdigest(), expected["sha256"])
                if kind == "marpa":
                    validate_marpa(text)
        for unsupported in ["START ::= Space Element Space", "START ::= A\nA ::= START", "ObjectTail ::= | ',' WS Member ObjectTail"]:
            bad = ":start ::= START\nevent ROOT_DONE = completed START\n" + unsupported
            with self.assertRaises(ValueError):
                validate_marpa(bad)

    def test_independent_boundary_contracts(self):
        for row in base.correctness_cases() + additional.correctness_cases():
            if row["subject"].isascii():
                self.assertEqual(generate.accepts(row["family"], row["subject"]), row["expected"], row["id"])
        for text, expected in practical.json_boundary_cases():
            self.assertEqual(generate.accepts("restricted_json", text), expected)
        self.assertFalse(ascii_json.accepts('"\x7f"'))
        self.assertTrue(ascii_json.accepts('"\\u007f"'))

    def test_public_fresh_variants_and_cfi(self):
        entries = generate.entries("practical")
        self.assertEqual(len(entries), 32)
        self.assertEqual(len({e["family"] for e in entries}), 8)
        for entry in entries:
            variants = [generate.fresh(entry, i) for i in [*range(24), 127, 640, 1922]]
            self.assertEqual(len(set(variants)), 27, entry["id"])
            blob = generate.cfi(variants[:8], entry["expected"])
            self.assertEqual(blob[:4], b"CFI1")
            self.assertEqual(struct.unpack_from("<I", blob, 4)[0], 8)
            self.assertTrue(all(generate.accepts(entry["family"], s) == entry["expected"] for s in variants[:8]))

    @unittest.skipUnless(importlib.util.find_spec("resource"), "Linux campaign controller")
    def test_status_boundaries(self):
        import run
        self.assertEqual(run.classify(0, {"status": "ok"}), "ok")
        self.assertEqual(run.classify(5, {"status": "resource"}), "resource")
        self.assertEqual(run.classify(-6, {}), "crash")
        self.assertEqual(run.classify(-9, {}, timed_out=True), "timeout")
        self.assertEqual(run.classify(-15, {}, cancelled=True), "cancelled")
        self.assertEqual(run.classify(0, {}), "protocol_error")
        # A Boolean mismatch invalidates the pair instead of manufacturing NoMatch.
        with self.assertRaises(ValueError):
            run.validate({"warm_batches": [{"count": 1}], "verdicts": [0, 0], "queried_records": 2},
                         "auto", True, 2, 1, 1)


if __name__ == "__main__":
    unittest.main()
