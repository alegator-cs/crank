#!/usr/bin/env python3
"""Native/DFA, capture, forward-equality and count regressions."""
import argparse
import importlib.util
import json
from pathlib import Path
import random
import re
import subprocess
import time

BASE = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("reference_tests", BASE / "reference/test_differential.py")
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


def run(binary, cases, args=()):
    data = "".join(f"{p.hex()}\t{s.hex()}\t{start}\n" for p, s, start in cases)
    done = subprocess.run([str(binary), "--batch", *args], input=data, text=True,
                          capture_output=True, timeout=180)
    if done.returncode or done.stderr:
        raise AssertionError(f"process failure {done.returncode}: {done.stderr[-4000:]}")
    lines = done.stdout.splitlines()
    assert len(lines) == len(cases), (len(lines), len(cases))
    return lines


def expected(pattern, subject, start):
    regex = re.compile(pattern, re.ASCII)
    ends = [j for j in range(start, len(subject) + 1) if regex.fullmatch(subject[start:j])]
    return str(int(bool(regex.fullmatch(subject)))) + "\t" + ",".join(map(str, ends))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    binary = args.binary.resolve()
    started = time.monotonic()
    texts = reference.words(b"ab", 4 if args.quick else 5)
    curated = [b"", b"a*", b"a*a", b"a*b*a", b"a*b*", b"a{0,2}b*", b"a{2,2}a*",
               b"(ab|a)*b", b"((a|b)*(c|d)*)*", b"(a*b){2}", b"(a|aaa){2}",
               b"(a|aaa){1,2}", b"(a?)*", b"(a*)*", b"((a)|b)*", b"(ab|ba)*",
               rb"\d+", rb"[a-z]+:[0-9]+", b"[^a]*", b"a{100000}",
               b"(a?){18446744073709551615}"]
    # Python cannot compile the last huge count; its language is checked
    # explicitly in the manual cases below.
    regular = [(p, s, 0) for p in curated[:-1] for s in texts]
    rng = random.Random(20260908)
    for p in reference.generated_patterns(30 if args.quick else 180):
        for s in rng.sample(texts, min(len(texts), 16)):
            regular.append((p, s, rng.randrange(len(s) + 1)))
    regular += [(b"((a|b)*(c|d)*)*", s, 0) for s in reference.words(b"abcd", 3)]
    truth = [expected(*case) for case in regular]
    counts = {}
    for backend in ["auto", "occurrences"]:
        actual = run(binary, regular, ["--backend", backend])
        for case, want, got in zip(regular, truth, actual):
            assert want == got, (backend, case, want, got)
        counts[backend] = len(regular)
    # Deliberately large repetitions exceed DFA compilation limits and use the
    # native fallback under auto; forced DFA is only checked where compilable.
    dfa_cases = [c for c in regular if c[0] != b"a{100000}"]
    for backend in ["dfa"]:
        cases = dfa_cases
        actual = run(binary, cases, ["--backend", backend, "--no-prune"])
        for case, got in zip(cases, actual):
            want = expected(*case)
            assert want == got, (backend, case, want, got)
        counts[backend] = len(cases)
    no_prune = run(binary, regular, ["--no-prune"])
    assert no_prune == truth, "optimization changed ordinary membership or endpoints"
    counts["unpruned"] = len(regular)

    backward_patterns = [rb"(a|b)\1", rb"(a*)\1", rb"(ab|a)\1", rb"(a)?\1",
                         rb"((a|b)*)\1", rb"(a*)(b*)\2\1", rb"((a)|b)*\2",
                         rb"(a|b)+\1", rb"(a?)*\1", rb"(a)(?:b|a)*\1"]
    backward = [(p, s, 0) for p in backward_patterns for s in texts]
    for options in [[], ["--no-prune"]]:
        for case, got in zip(backward, run(binary, backward, options)):
            want = expected(*case)
            assert got == want, ("backward", options, case, want, got)

    # For regular syntax, all-outcome matching must include Python's selected
    # capture result. Additional outcomes are intentional, not a priority claim.
    capture_patterns = [b"(a|b)*", b"((a)|b)*", b"(a*)a*", b"(a?)*", b"(a*)*",
                        b"(a)?b*", b"(a*b){2}"] + backward_patterns
    capture_cases = [(p, s, 0) for p in capture_patterns for s in texts[:31]]
    for case, line in zip(capture_cases, run(binary, capture_cases, ["--captures"])):
        p, s, _ = case
        got = json.loads(line)
        match = re.fullmatch(p, s, re.ASCII)
        assert bool(got) == bool(match), ("capture acceptance", case, got)
        if match:
            wanted = [None if match.start(i) == -1 else (match.start(i), match.end(i))
                      for i in range(len(match.groups()) + 1)]
            actual_spans = [[None if c is None else (c["begin"], c["end"]) for c in x["captures"]]
                            for x in got]
            assert wanted in actual_spans, ("selected capture missing", case, wanted, got)

    manual = []
    for s in reference.words(b"ab", 4):
        manual += [(rb"\k<x>(?<x>a|b)", s, s in [b"aa", b"bb"]),
                   (rb"\1(a|b)", s, s in [b"aa", b"bb"]),
                   (rb"(?:\k<x>(?<x>a|b))*", s,
                    len(s) % 2 == 0 and all(s[i] == s[i+1] for i in range(0, len(s), 2))),
                   (rb"\k<x>\k<x>(?<x>a*)", s, set(s) <= {97} and len(s) % 3 == 0)]
    for n in range(6):
        good = b"a"*n + b"b"*n + b"c"*n
        manual.append((b"a{#n}b{=n}c{=n}", good, True))
        manual.append((b"a{#n}b{=n}c{=n}", good + b"a", False))
        manual.append((b"a{#n}b{=n}c{=n}", b"a"*n+b"b"*(n+1)+b"c"*n, False))
    manual += [(b"(?:a{#n}b{=n})*", b"abaabb", True),
               (b"(?:a{#n}b{=n})*", b"aab", False),
               (b"a{#n}b{#m}c{=n}d{=m}", b"aabccd", True),
               (rb"(?<x>a|b)\k<x>", b"ab", False),
               (rb"\k<x>(?:(?<x>a)|b)", b"b", False),
               (b"(a?){18446744073709551615}", b"aaa", True)]
    manual_cases = [(p, s, 0) for p, s, _ in manual]
    for options in [[], ["--no-prune"]]:
        for item, line in zip(manual, run(binary, manual_cases, options)):
            assert not line.startswith(("ERR", "LIMIT")), (item, line)
            assert (line.split('\t')[0] == "1") == item[2], (options, item, line)
    count_output = json.loads(run(binary, [(b"a{#n}b{=n}c{=n}", b"aaabbbccc", 0)], ["--captures"])[0])
    assert len(count_output) == 1 and count_output[0]["counts"] == {"n": 3}, count_output

    rejected = [rb"\1", rb"(\1)", rb"(?<x>\k<x>)", rb"(?<x>a)(?<x>b)",
                rb"\k<missing>a", b"a{=n}b{#n}", b"(a?){#n}", b"a{#n}b{#n}",
                b"a{#n}(b?){=n}", b"a{1,0}", b"a{18446744073709551616}",
                b"[[:alpha:]]", b"(?=a)", b"a*?", rb"\q"]
    for case, got in zip(rejected, run(binary, [(p, b"", 0) for p in rejected])):
        assert got.startswith("ERR\t"), ("expected rejection", case, got)
    limited = run(binary, [(b"(a|b)*", b"abab", 0)], ["--captures", "--max-states", "1"])[0]
    assert limited.startswith("LIMIT\t"), limited
    assert run(binary, [(b"a{100000}", b"", 0)], ["--backend", "dfa"])[0].startswith("LIMIT\t")
    assert run(binary, [(rb"(a)\1", b"a", 0)], ["--backend", "dfa"])[0].startswith("ERR\t")
    assert run(binary, [(rb"(a)\1", b"a", 2)])[0].startswith("ERR\t")
    assert run(binary, [(b"(a)", b"a", 0)], ["--captures", "--backend", "dfa"])[0].startswith("ERR\t")

    # Deep source nesting preserves DFA and native endpoint semantics.
    deep = b"a"
    for _ in range(256): deep = b"(" + deep + b"b)*"
    for backend in ["dfa", "occurrences"]:
        assert run(binary, [(deep, b"", 0)], ["--backend", backend])[0] == "1\t0"

    # Greedy certificates must not be inferred from FIRST/FIRST alone.
    for p in [b"(ab|a)*b", b"a*b*a"]:
        got = run(binary, [(p, b"ab" if p.startswith(b"(") else b"a", 0)],
                  ["--backend", "greedy", "--no-prune"])[0]
        assert got.startswith("ERR\t"), (p, got)
    print(json.dumps({"status": "PASS", "regular_checks": counts,
                      "backreference_cases_each_prune_mode": len(backward),
                      "capture_outcome_cases": len(capture_cases),
                      "forward_and_count_cases_each_prune_mode": len(manual),
                      "invalid_syntax_cases": len(rejected), "resource_limit_cases": 2,
                      "backend_and_offset_rejections": 3, "deep_nesting_cases": 2,
                      "quick": args.quick, "seconds": round(time.monotonic()-started,3)}, indent=2))


if __name__ == "__main__":
    main()
