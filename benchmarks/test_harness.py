#!/usr/bin/env python3
"""Light harness checks; these are not engine measurements."""
import json
import unittest
from fixtures import cases, lattice_possible
from run import parse_protocol
from plot import summarize


class HarnessTests(unittest.TestCase):
    def test_assistance_original_bytes_and_independent_oracles(self):
        rows = cases("assistance")
        self.assertEqual(len(rows), 4)
        self.assertEqual([r["expected"] for r in rows], [False, True, False, True])
        self.assertEqual(len(rows[0]["subject"]), 8197)
        self.assertEqual(len(rows[2]["subject"]), 131040)
    def test_practical_original_hashes_and_independent_oracles(self):
        rows = cases("practical")
        self.assertEqual(len(rows), 28)
        self.assertEqual(len({r["group"] for r in rows}), 7)
        self.assertEqual(sum(r["expected"] for r in rows), 7)
    def test_original_rejections_and_positive_controls(self):
        fixtures = {c["id"]: c for c in cases("rejection")}
        for name in ("lattice_16379", "bounded_lattice_8191", "bounded_lattice_16379", "record_flat_131072_final_digit"):
            self.assertFalse(fixtures[name]["expected"])
        for name in ("lattice_16380_positive", "bounded_lattice_8192_positive", "bounded_lattice_16254_positive", "record_flat_131072_positive"):
            self.assertTrue(fixtures[name]["expected"])
        self.assertEqual(len(fixtures["record_flat_131072_final_digit"]["subject"]), 131064)
        self.assertEqual(fixtures["record_flat_131072_final_digit"]["subject"][-2:], b"!\n")
        self.assertTrue(lattice_possible(127 * 63 + 131 * 63, 63))
        self.assertFalse(lattice_possible(127 * 64, 63))

    def protocol(self, result, prepared=True, rc=0, iterations=0):
        rows = ([dict(stage="prepared", version="test")] if prepared else []) + [result]
        return parse_protocol("\n".join(json.dumps(r) for r in rows), rc, False, "measure", iterations, 1)

    def complete(self):
        return dict(stage="result", status="complete", matched=False, prepare_ns=1, first_query_ns=2,
                    iterations=20, warm_wall_ns_per_query=[3], warm_cpu_ns_per_query=[None], warm_batch_ns=[60])

    def test_nullable_cpu_and_calibrated_iterations(self):
        self.assertEqual(self.protocol(self.complete())["status"], "complete")

    def test_wrong_verdict_and_malformed_samples(self):
        result = self.complete(); result["matched"] = True
        self.assertEqual(self.protocol(result)["status"], "incorrect")
        result = self.complete(); result["warm_cpu_ns_per_query"] = [-1]
        self.assertEqual(self.protocol(result)["status"], "protocol_error")
        result = self.complete(); result["warm_batch_ns"] = [61]
        self.assertEqual(self.protocol(result)["status"], "protocol_error")
        self.assertEqual(self.protocol(self.complete(), iterations=10)["status"], "protocol_error")
        self.assertEqual(self.protocol(self.complete(), rc=1)["status"], "protocol_error")

    def test_limits_and_unsupported_are_not_no_match(self):
        for status in ("resource_limit", "unsupported", "unavailable"):
            result = dict(stage="result", status=status, matched=None)
            self.assertEqual(self.protocol(result, prepared=False)["status"], status)
            result["matched"] = False
            self.assertEqual(self.protocol(result)["status"], "protocol_error")
        self.assertEqual(self.protocol(dict(stage="result", status="unsupported"))["status"], "protocol_error")

    def test_plot_excludes_nondecision_and_uses_process_medians(self):
        report = dict(check_only=False, gate="passed for completed checks; nondecisions excluded from timing", finished_utc="test", measurement=dict(repeats=3), measurements=[])
        for values in ([1, 100, 3], [5, 1000, 7]):
            row = self.complete(); row.update(fixture="x", engine="crank", warm_wall_ns_per_query=values, warm_batch_ns=[x*20 for x in values])
            report["measurements"].append(row)
        report["measurements"].append(dict(fixture="x", engine="crank", status="resource_limit", matched=None))
        row = summarize(report)[0]
        self.assertEqual(row["warm_process_medians_ns"], [3, 7])
        self.assertIsNone(row["warm_median_ns"])
        self.assertEqual(row["completed_processes"], 2)
        report["gate"] = "measurement verdict/protocol failure; partial campaign invalid"
        with self.assertRaises(ValueError): summarize(report)


if __name__ == "__main__":
    unittest.main()
