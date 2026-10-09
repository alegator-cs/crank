#!/usr/bin/env python3
"""Public CLI contracts for occurrence paths, recursive metrics, and traces."""
import json
from pathlib import Path
import subprocess
import sys

binary = str(Path(sys.argv[1]).resolve())
checks = 0


def run(args, code):
    global checks
    result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=30)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    checks += 1
    return result.stdout


def traced(pattern, subject, code=0):
    result = json.loads(run(['--trace', pattern, subject], code))
    assert set(result) == {'matches', 'occurrences', 'statistics'}
    assert result['statistics']['backend'] == 'occurrences-trace'
    assert result['statistics']['limit_reached'] is False
    if code == 1:
        assert result['matches'] == [] and result['occurrences'] == []
        return result
    assert len(result['matches']) == 1
    assert result['matches'][0]['captures'][0] == {
        'begin': 0, 'end': len(subject), 'text': subject}
    records = result['occurrences']
    assert records[0]['kind'] == 'root' and records[0]['parent'] is None
    for index, record in enumerate(records):
        assert record['id'] == index and record['closed'] is True
        assert 0 <= record['begin'] <= record['end'] <= len(subject)
        assert record['bytes'] == record['end'] - record['begin']
        for child in record['children']:
            assert child > index and records[child]['parent'] == index
    return result


rows = r'(?:a{#cols};){#rows}b{=rows[1].cols}'
assert '[occurrences]' in run([rows, 'a;aaa;aa;bbb'], 0)
run([rows, 'a;aaa;aa;bb'], 1)
run(['--no-prune', rows, 'a;aaa;aa;bbb'], 0)
row_trace = traced(rows, 'a;aaa;aa;bbb')
assert row_trace['matches'][0]['counts'] == {'cols': 2, 'rows': 3}
assert [record['count'] for record in row_trace['occurrences']
        if record['kind'] == 'count' and record['name'] == 'cols'] == [1, 3, 2]
traced(rows, 'a;aaa;aa;bb', 1)

recursive = r'(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}'
proof = traced(recursive, 'aaabbbccc')
invocations = [record for record in proof['occurrences']
               if record['kind'] == 'group' and record['name'] == 'S']
assert [(record['depth'], record['height'], record['begin'], record['end'])
        for record in invocations] == [(0, 3, 0, 6), (1, 2, 1, 5), (2, 1, 2, 4), (3, 0, 3, 3)]
assert all(record['source_rule'] for record in invocations)
assert proof['statistics']['down_steps'] > 0 and proof['statistics']['up_steps'] > 0
run(['--backend', 'occurrences', recursive, 'aaabbbccc'], 0)
assert 'Endpoints: 0 3' in run(['--endpoints', recursive, 'abcx'], 1)

for mode in ['--first', '--captures']:
    result = json.loads(run([mode, recursive, 'aaabbbccc'], 0))
    assert result and result[0]['captures'][1] == {'begin': 0, 'end': 6, 'text': 'aaabbb'}
for flag in ['--first', '--captures']:
    run(['--trace', flag, rows, 'a;aaa;aa;bbb'], 2)
for backend in ['dfa', 'states', 'greedy', 'pcre2']:
    run(['--backend', backend, recursive, 'aaabbbccc'], 2)

for cap in ['--max-states', '--max-steps', '--max-occurrences', '--max-call-depth',
            '--max-occurrence-states', '--max-occurrence-steps']:
    run([cap, '0', recursive, 'aaabbbccc'], 3)
for cap in ['--max-repeat', '--max-occurrence-repeat']:
    run([cap, '1', r'a{#n:2,4}b{=#n}', 'aaabbb'], 3)
epsilon = r'(?<S>(?&S)|)c{=S[0].@bytes}'
run(['--max-call-depth', '4', epsilon, ''], 0)
run(['--max-call-depth', '4', epsilon, 'a'], 3)
run(['--captures', '--max-call-depth', '4', epsilon, ''], 3)

for malformed in [r'a{#n:3,2}', r'a{#n:1,2}b{=n[1]}', r'(?<G>a)b{=G[0].@count}',
                  r'a{#n:1,2}b{=n.@height}', r'a{#n:1,2}b{=#99}']:
    run([malformed, ''], 2)

assert '[native-ast]' in run(['--backend', 'occurrences', '(?:ab)*', 'abab'], 0)
traced('(a)b', 'ab')
print(json.dumps({'status': 'PASS', 'occurrence_cli_checks': checks}, sort_keys=True))
