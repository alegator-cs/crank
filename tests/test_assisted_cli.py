#!/usr/bin/env python3
"""Check observable assisted CLI contracts without scheduling assumptions."""
import json
import subprocess
import sys

binary = sys.argv[1]
checks = 0

def run(args, status, code):
    global checks
    p = subprocess.run([binary, *args], capture_output=True, text=True, timeout=30)
    assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
    value = json.loads(p.stdout)
    assert value["status"] == status, value
    assert value["matched"] == {"match": True, "no_match": False}.get(status), value
    assert value["query_us"] >= 0 and value["drain_us"] >= 0
    checks += 1
    return value

run(["--", "(a|b)*", "abab"], "match", 0)
run(["--", "a+", "b"], "no_match", 1)
v = run(["--threads", "1", "--", "a*ab", "aaaaab"], "match", 0)
assert v["decision"] == "disabled" and v["statistics"]["checks"] == 0
v = run(["--min-helper-bytes", "0", "--owned-input", "--drain", "--", "a*ab", "aaaab"], "match", 0)
assert v["statistics"]["copied_input_bytes"] == 0
assert v["pool"]["active_helpers"] == 0
assert v["pool"]["tasks_finished"] == v["pool"]["tasks_submitted"]
v = run(["--min-helper-bytes", "0", "--", "a+", "aaa"], "match", 0)
assert v["decision"] == "below_work_threshold"
assert v["statistics"]["helpers_admitted"] == 0
run(["--threads", "1", "--max-steps", "0", "--", "a+", "aaa"], "resource_limit", 3)
run(["--threads", "0", "--", "a", "a"], "error", 2)
run(["--helper-steps", "-1", "--", "a", "a"], "error", 2)
run(["--threads"], "error", 2)
run(["--", "[", "a"], "error", 2)
run(["--unknown", "a", "a"], "error", 2)
run(["--", r"(a)\1", "aa"], "error", 2)
for policy in ("always", "selective", "progress"):
    run(["--assistance-policy", policy, "--min-helper-bytes", "0", "--drain", "--", "a+", "aaa"], "match", 0)
v = run(["--assistance-policy", "progress", "--min-helper-bytes", "0", "--min-native-steps", "999999", "--", "a+", "aaa"], "match", 0)
assert v["decision"] == "below_work_threshold"
assert v["statistics"]["copied_input_bytes"] == 0
assert v["statistics"]["helpers_admitted"] == 0
run(["--assistance-policy", "bad", "--", "a", "a"], "error", 2)
run(["--min-native-steps", "-1", "--", "a", "a"], "error", 2)
print(f"assisted CLI passed {checks} checks")
