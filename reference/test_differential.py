#!/usr/bin/env python3
"""Compare Boolean matches and ALL endpoints to Python re on a shared byte subset."""
import argparse
import itertools
import json
import random
import re
import subprocess
import time
from pathlib import Path


def words(alphabet, limit):
    return [b"".join(chars) for n in range(limit + 1)
            for chars in itertools.product([bytes([x]) for x in alphabet], repeat=n)]


def generated_patterns(count):
    rng = random.Random(20260907)
    atoms = ["a", "b", "[ab]", "[^a]", ".", "(?:)"]

    def tree(depth):
        if depth == 0 or rng.random() < 0.28:
            return rng.choice(atoms)
        kind = rng.randrange(3)
        if kind == 0:
            return "(?:" + tree(depth - 1) + tree(depth - 1) + ")"
        if kind == 1:
            return "(?:" + tree(depth - 1) + "|" + tree(depth - 1) + ")"
        return "(?:" + tree(depth - 1) + ")" + rng.choice(
            ["*", "+", "?", "{0}", "{2}", "{0,2}", "{1,3}", "{2,}"])

    return [tree(3).encode() for _ in range(count)]


def run(binary, cases):
    data = "".join(f"{p.hex()}\t{s.hex()}\t{start}\n" for p, s, start in cases)
    process = subprocess.run([str(binary), "--batch"], input=data,
                             capture_output=True, text=True, timeout=120)
    if process.returncode:
        raise AssertionError(f"matcher exited {process.returncode}: {process.stderr[-5000:]}")
    if process.stderr:
        raise AssertionError(process.stderr)
    results = process.stdout.splitlines()
    if len(results) != len(cases):
        raise AssertionError(f"expected {len(cases)} responses, got {len(results)}")
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    binary = args.binary.resolve()
    started = time.monotonic()
    curated = [
        b"", b"a", b"ab", b"abc", b"a|b", b"ab|ba", b"a|bc", b"(a|b)*",
        b"((a|b)*(c|d)*)*", b"(a|bb)*", b"(ab)*", b"(a)*", b"(a|b){2}", b"(a*b){2}",
        b"(ab|a)b", b"(ab|a)*b", b"a*a", b"a*b*a", b"a*b*", b"a*b", b"a*a*",
        b"a?", b"a{0}", b"a{2}", b"a{2,2}", b"a{1,3}", b"a{2,}",
        b"(a?)*", b"(a*)*", b"(a?)+", b"(a?){2}", b"(a?){2,3}", b"(a*){2,}",
        b"(a|)*", b"(|a)+", b"a|", b"|a", b"()", b"()*", b"(){3}", b"(?:ab)*",
        b"((ab)?|a)*b", b"(a*b*)*", b"[ab]*", b"[^a]*", b"[a-c]{1,3}", b".*",
        rb"\d+", rb"\w*", rb"\s*", rb"\D+", rb"\W*", rb"\S*", rb"\x61+",
        rb"a\+b", rb"[\dA-C]+", rb"[\x00-\x02]*", rb"\x00*", rb"[\-a]*",
        b"[]]*", b"[^]]*", b"[a-]*", rb"[\xff]*",
    ]
    texts = words(b"ab", 4 if args.quick else 5)
    texts += [b"abc", b"aabdcabdc", b"dcab", b"acbd", b"xyz", b"7", b"12a", b"_",
              b" ", b"\n", b"\t", b"\x00", b"\x00\x01", b"\xff", b"A", b"a+b", b"]", b"-"]
    cases = [(p, s, 0) for p in curated for s in texts]
    # Nonzero starts verify memo keys and absolute offsets, including end-of-input.
    cases += [(p, s, start) for p in curated[:42] for s in words(b"ab", 3)
              for start in range(1, len(s) + 1)]
    rng = random.Random(731)
    for pattern in generated_patterns(40 if args.quick else 350):
        for subject in rng.sample(texts, min(len(texts), 24)):
            cases.append((pattern, subject, rng.randrange(len(subject) + 1)))
    if not args.quick:
        cases += [(b"((a|b)*(c|d)*)*", s, 0) for s in words(b"abcd", 5)]

    results = run(binary, cases)
    compiled = {}
    for (pattern, subject, start), actual in zip(cases, results):
        oracle = compiled.setdefault(pattern, re.compile(pattern, re.ASCII))
        matched = bool(oracle.fullmatch(subject))
        ends = [end for end in range(start, len(subject) + 1)
                if oracle.fullmatch(subject[start:end])]
        expected = f"{int(matched)}\t" + ",".join(map(str, ends))
        if actual != expected:
            raise AssertionError(f"pattern={pattern!r}, subject={subject!r}, start={start}: "
                                 f"expected {expected!r}, got {actual!r}")

    # Huge nullable counts need semantic tests outside Python's quantifier limit.
    large = b"18446744073709551615"
    manual = [
        ((b"(a?){" + large + b"}", b"aaa", 0), "1\t0,1,2,3"),
        ((b"(){" + large + b"}", b"", 0), "1\t0"),
        ((b"a{" + large + b"}", b"aaa", 0), "0\t"),
        ((b"(a?){" + large + b",}", b"aa", 1), "1\t1,2"),
        ((b"(?:ab|){2}", b"ababab", 0), "0\t0,2,4"),
    ]
    for (case, expected), actual in zip(manual, run(binary, [x[0] for x in manual])):
        if actual != expected:
            raise AssertionError(f"manual {case!r}: expected {expected!r}, got {actual!r}")

    rejected = [b"(", b")", b"[a", b"[]", b"[z-a]", b"*a", b"a**", b"a*?", b"a++",
                b"a{", b"a{}", b"a{2,1}", b"a{,2}", b"a{1,2,3}",
                b"a{18446744073709551616}", bytes([92]), rb"\q", rb"\1", rb"(a)\1", rb"\0",
                rb"\x0", rb"\xZZ", b"^a", b"a$", b"(?=a)", b"(?<name>a)",
                b"[[:alpha:]]", b"[[.a.]]", b"[[=a=]]"]
    rejected_cases = [(p, b"", 0) for p in rejected] + [(b"a", b"a", 2)]
    for case, actual in zip(rejected_cases, run(binary, rejected_cases)):
        if not actual.startswith("ERR\t"):
            raise AssertionError(f"expected explicit rejection of {case!r}: {actual!r}")

    summary = {
        "oracle": "Python re, byte patterns, ASCII classes; all accepted endpoints checked",
        "differential_cases": len(cases), "distinct_patterns": len(compiled),
        "manual_large_count_cases": len(manual), "rejection_cases": len(rejected_cases),
        "seed": 20260907, "quick": args.quick,
        "seconds": round(time.monotonic() - started, 3), "status": "PASS",
    }
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
