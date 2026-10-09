#!/usr/bin/env python3
"""Public CLI contracts for recursive membership, witnesses and optional PCRE2."""
import json
from pathlib import Path
import subprocess
import sys

binary = str(Path(sys.argv[1]).resolve())
pcre = '--pcre2' in sys.argv[2:]
checks = 0


def run(args, code):
    global checks
    r = subprocess.run([binary, *args], capture_output=True, text=True, timeout=30)
    assert r.returncode == code, (args, r.returncode, r.stdout, r.stderr)
    checks += 1
    return r.stdout


balanced = r'(?(DEFINE)(?<S>\((?&S)\)(?&S)|))(?&S)'
for subject in ['', '()', '(())', '()()', '(()())()']:
    assert '[native-ast]' in run([balanced, subject], 0)
    result = json.loads(run(['--first', balanced, subject], 0))
    assert len(result) == 1 and result[0]['captures'][0] == {
        'begin': 0, 'end': len(subject), 'text': subject}
for subject in ['(', ')', '())(', '(a)']:
    run([balanced, subject], 1)
    assert json.loads(run(['--first', balanced, subject], 1)) == []
run(['--captures', balanced, '()'], 2)
run(['--first', '--captures', balanced, '()'], 2)
run(['--backend', 'states', balanced, '()'], 2)
# Retired implementations are not selectable in the public runtime.
for retired in ['grammar', 'parallel', 'reference']:
    run(['--backend', retired, balanced, '()'], 2)
# Acyclic call spelling exposes the same regular language without AST inlining.
called = r'(?(DEFINE)(?<A>a|ab))(?&A)b|(?&A)c'
for subject in ['ab', 'abb', 'ac', 'abc']:
    assert '[dfa]' in run([called, subject], 0)
    assert '[dfa]' in run(['--backend', 'dfa', called, subject], 0)
for subject in ['', 'a', 'abac', 'acb']:
    run([called, subject], 1)
assert 'Endpoints: 2 3' in run(['--endpoints', called, 'abc'], 0)
assert '[native-ast]' in run(['--max-dfa-states', '0', called, 'abc'], 0)
run(['--max-dfa-states', '0', '--backend', 'dfa', called, 'abc'], 3)
called_capture = json.loads(run(['--first', called, 'abc'], 0))
assert called_capture[0]['captures'][1]['text'] == 'ab'
# A bounded DFA compiler falls back to the same native recognizer used by CFGs.
assert '[native-ast]' in run(['--max-dfa-states', '0', '(ab|ba)*', 'abba'], 0)
assert 'Endpoints: 0 2 4' in run(['--max-dfa-states', '0', '--endpoints', '(ab|ba)*', 'abbax'], 1)
run(['--max-dfa-states', '0', '--backend', 'dfa', '(ab|ba)*', 'abba'], 3)
run(['--max-dfa-states', '0', '--max-states', '0', '(ab|ba)*', 'abba'], 3)
assert '[occurrences]' in run(['--no-call-sharing', balanced, '()'], 0)
run(['--max-states', '0', balanced, '()'], 3)
run(['--max-steps', '0', balanced, '()'], 3)
assert 'Endpoints: 0 2 4' in run(['--endpoints', balanced, '()()x'], 1)
for mixed, valid in [(r'(?<A>a(?&A)?)(b)\2', 'aabb'),
                     (r'(?<A>a(?&A)?){#n}b{=n}', 'aabb')]:
    assert '[occurrences]' in run([mixed, valid], 0)
    run([mixed, 'a'], 1)
left = r'(?(DEFINE)(?<E>(?&E)\+[0-9]+|[0-9]+))(?&E)'
run([left, '1+23+456'], 0)
run([left, '1++23'], 1)
run([r'a(?R)?b', 'aaabbb'], 0)
for pattern, subject in [(r'(a|b)\1', 'bb'), (r'a{#n}b{=n}', 'aaabbb')]:
    all_results = json.loads(run(['--captures', pattern, subject], 0))
    one = json.loads(run(['--first', pattern, subject], 0))
    assert len(one) == 1 and one[0] in all_results
run(['--backend', 'pcre2', '(?:ab|ba)*', 'abba'], 0 if pcre else 2)
if pcre:
    assert 'pcre2-or-dfa' in run(['--backend', 'pcre2', '(?:ab|ba)*', 'abba'], 0)
    assert 'Endpoints: 0 2 4' in run(['--backend', 'pcre2', '--endpoints', '(?:ab|ba)*', 'abba'], 0)
print(json.dumps({'status': 'PASS', 'capability_cli_checks': checks,
                  'pcre2_enabled': pcre}, sort_keys=True))
