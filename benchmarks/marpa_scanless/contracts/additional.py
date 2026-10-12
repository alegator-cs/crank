"""Public synthetic CSV/INI/XML/arithmetic; historical grammar bytes and oracles."""
from __future__ import annotations
from .grammar_sources import pair_map
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BASE = ROOT.parents[1]
CRANK_GRAMMARS = pair_map(["csv", "ini", "xml_subset", "arithmetic_expression"], "crank")

MARPA_GRAMMARS = pair_map(["csv", "ini", "xml_subset", "arithmetic_expression"], "marpa")

SCOPES = {
    "csv": "ASCII CSV syntax, one or more LF/CRLF-terminated records; empty fields; printable unquoted bytes except comma/quote; quoted printable bytes plus tab/CR/LF, quotes escaped by doubling. No column-count/schema obligation, encoding detection, or data conversion.",
    "ini": "One or more LF/CRLF-terminated ASCII settings lines: blanks, ;/# comments, named sections with optional trailing comment, or key=value assignments. Keys and section names have the explicit grammar alphabets. Values are uninterpreted printable ASCII/tab. No section merging, duplicate-key policy, value typing, or configuration evaluation.",
    "xml_subset": "ASCII XML-shaped document with exactly one element, matched fixed-vocabulary tags root/node/item/group, double-quoted attributes, mixed text/child content, self-closing elements and five named XML entities. No declarations, namespaces, numeric entities, DTD/schema, attribute uniqueness, or DOM construction. Closing tags match via finite grammar alternatives, not backreferences or a semantic action.",
    "arithmetic_expression": "Nonempty ASCII arithmetic expression with identifiers, unsigned integers/decimal literals, unary +/- signs, parentheses, conventional * / precedence above + -, whitespace allowed at token boundaries. Boolean recognition only: no evaluation, variable lookup, division checks, AST, captures, or preferred-parse output.",
}

def sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()

def _csv(text: str) -> bool:
    n, i, records = len(text), 0, 0
    while i < n:
        while True:
            if i < n and text[i] == '"':
                i += 1
                while i < n:
                    if text[i] == '"':
                        if i + 1 < n and text[i+1] == '"':
                            i += 2
                            continue
                        i += 1
                        break
                    if text[i] not in '\t\r\n' and not (32 <= ord(text[i]) <= 126):
                        return False
                    i += 1
                else:
                    return False
            else:
                while i < n and text[i] not in ',\r\n':
                    if text[i] == '"' or not (32 <= ord(text[i]) <= 126):
                        return False
                    i += 1
            if i < n and text[i] == ',':
                i += 1
                continue
            if i < n and text[i] == '\r':
                i += 1
            if i >= n or text[i] != '\n':
                return False
            i += 1
            records += 1
            break
    return records > 0

def _ini(text: str) -> bool:
    if not text or not text.endswith('\n'):
        return False
    for physical in text.splitlines(keepends=True):
        if not physical.endswith('\n'):
            return False
        line = physical[:-1]
        if line.endswith('\r'):
            line = line[:-1]
        if any(c != '\t' and not 32 <= ord(c) <= 126 for c in line):
            return False
        line = line.lstrip(' \t')
        if not line or line[0] in ';#':
            continue
        if line[0] == '[':
            if ']' not in line:
                return False
            name, tail = line[1:].split(']', 1)
            if not re.fullmatch(r'[A-Za-z0-9_.:/ @-]+', name):
                return False
            tail = tail.lstrip(' \t')
            if tail and tail[0] not in ';#':
                return False
        else:
            key, sep, _value = line.partition('=')
            if not sep or not re.fullmatch(r'[A-Za-z_:-][A-Za-z0-9_.:/ \t-]*', key):
                return False
    return True

class Invalid(ValueError):
    pass

class XMLOracle:
    def __init__(self, text: str):
        self.text, self.i = text, 0
    def take(self, literal: str) -> bool:
        if self.text.startswith(literal, self.i):
            self.i += len(literal)
            return True
        return False
    def need(self, literal: str):
        if not self.take(literal):
            raise Invalid(literal)
    def white(self):
        old = self.i
        while self.i < len(self.text) and self.text[self.i] in ' \t\r\n':
            self.i += 1
        return self.i > old
    def entity(self):
        for value in ('&amp;', '&lt;', '&gt;', '&quot;', '&apos;'):
            if self.take(value):
                return
        raise Invalid('entity')
    def name(self):
        m = re.match(r'[A-Za-z_][A-Za-z0-9_.:-]*', self.text[self.i:])
        if not m:
            raise Invalid('name')
        self.i += len(m.group())
    def element(self):
        self.need('<')
        tag = next((t for t in ('root', 'node', 'item', 'group')
                    if self.text.startswith(t, self.i)), None)
        if tag is None:
            raise Invalid('tag')
        self.i += len(tag)
        while True:
            space = self.white()
            if self.take('/>'):
                return
            if self.take('>'):
                break
            if not space:
                raise Invalid('attribute separator')
            self.name()
            self.white(); self.need('='); self.white(); self.need('"')
            while not self.take('"'):
                if self.i >= len(self.text):
                    raise Invalid('attribute EOF')
                ch = self.text[self.i]
                if ch == '&':
                    self.entity()
                elif 32 <= ord(ch) <= 126 and ch not in '<"':
                    self.i += 1
                else:
                    raise Invalid('attribute byte')
        while not self.take('</' + tag + '>'):
            if self.i >= len(self.text):
                raise Invalid('element EOF')
            ch = self.text[self.i]
            if ch == '<':
                self.element()
            elif ch == '&':
                self.entity()
            elif ch in '\t\r\n' or (32 <= ord(ch) <= 126 and ch not in '<>'):
                self.i += 1
            else:
                raise Invalid('text byte')

def _xml(text: str) -> bool:
    p = XMLOracle(text)
    try:
        p.white(); p.element(); p.white()
        return p.i == len(text)
    except (Invalid, RecursionError):
        return False

def _arithmetic(text: str) -> bool:
    # Independent ordinary recursive-descent recognition; no Crank AST/shared parser.
    tokens, at = [], 0
    token = re.compile(r'(?:[0-9]+\.[0-9]+|[0-9]+|[A-Za-z_][A-Za-z0-9_]*|[()+*/-])')
    while at < len(text):
        if text[at] in ' \t\r\n':
            at += 1
            continue
        m = token.match(text, at)
        if not m:
            return False
        tokens.append(m.group()); at = m.end()
    i = 0
    def factor():
        nonlocal i
        while i < len(tokens) and tokens[i] in ('+', '-'):
            i += 1
        if i >= len(tokens):
            raise Invalid('operand')
        if tokens[i] == '(':
            i += 1; expression()
            if i >= len(tokens) or tokens[i] != ')':
                raise Invalid('close')
            i += 1
        elif re.fullmatch(r'(?:[0-9]+(?:\.[0-9]+)?|[A-Za-z_][A-Za-z0-9_]*)', tokens[i]):
            i += 1
        else:
            raise Invalid('operand')
    def term():
        nonlocal i
        factor()
        while i < len(tokens) and tokens[i] in ('*', '/'):
            i += 1; factor()
    def expression():
        nonlocal i
        term()
        while i < len(tokens) and tokens[i] in ('+', '-'):
            i += 1; term()
    try:
        expression()
        return i == len(tokens)
    except (Invalid, RecursionError):
        return False

def accepts(family: str, subject: str) -> bool:
    if not subject.isascii():
        return False
    return {'csv': _csv, 'ini': _ini, 'xml_subset': _xml,
            'arithmetic_expression': _arithmetic}[family](subject)

CSV_SEED = "case,name,value,notes,enabled,kind,level,owner,region\n" + "".join(
    'item%03d,name%03d,%d,"comma,quote ""escaped""",true,example,2,public,west\n' % (i, i, i)
    for i in range(48))
INI_SEED = "name=PPI_demo\nversion=1.0\nauthor=Example\n[settings]\n" + "".join(
    "option_%03d=value_%03d\n" % (i, i) for i in range(96))

def _load_sources():
    return {"csv": (Path("synthetic-csv"), CSV_SEED.encode("ascii")),
            "ini": (Path("synthetic-ini"), INI_SEED.encode("ascii"))}


def cases() -> list[dict]:
    source = _load_sources()
    csv_bytes = source['csv'][1]
    ini_bytes = source['ini'][1]
    csv_small = b','.join(csv_bytes.splitlines()[0].split(b',')[:9]) + b'\n'
    ini_small = b'\n'.join(ini_bytes.splitlines()[:3]) + b'\n'
    xml_small = '<root id="alpha0000"><item count="12">green &amp; blue</item><node/><group><item>42</item></group></root>\n'
    xml_large = '<root id="alpha0000">\n' + ''.join(
        '<group name="cluster%02d"><node index="%02d"><item count="12">green &amp; blue</item><item>42</item></node><item/></group>\n' % (i, i)
        for i in range(33)) + '</root>\n'
    expr_small = 'var000 * (12.5 + rate_2) - -offset + (limit / 3) + 7 * (base - 2) + value_1 / (scale + 4) + tail00\n'
    expr_large = 'var000 + ' + ' + '.join(
        '((base%02d + 12.5) * gain_%02d - -offset%02d) / (scale_%02d + 4)' % (i, i, i, i)
        for i in range(64)) + ' + tail00\n'
    data = {'csv': (csv_small.decode('ascii'), csv_bytes.decode('ascii')),
            'ini': (ini_small.decode('ascii'), ini_bytes.decode('ascii')),
            'xml_subset': (xml_small, xml_large),
            'arithmetic_expression': (expr_small, expr_large)}
    result = []
    for family, texts in data.items():
        for size, valid in zip(('small', 'large'), texts):
            assert accepts(family, valid), (family, size)
            provenance = {"kind": "synthetic_public_fixture",
                          "description": "Deterministic public syntax fixture; no local files."}
            if family == 'csv':
                offset, replacement, mutation = len(valid)-1, '"', 'Replace final record LF with opening quote: incomplete final field/record.'
            elif family == 'ini':
                offset, replacement, mutation = valid.rfind('='), '[', 'Replace the final assignment delimiter = with [: malformed settings line.'
            elif family == 'xml_subset':
                offset, replacement, mutation = valid.rfind('</root>') + 5, 'X', 'Replace one final closing-root tag byte: mismatched closing tag.'
            else:
                offset, replacement, mutation = valid.rfind('tail00')+5, '+', 'Replace last operand byte with trailing binary +: missing right operand.'
            invalid = valid[:offset] + replacement + valid[offset+1:]
            assert len(invalid) == len(valid) and not accepts(family, invalid), (family, size, invalid[-80:])
            for expected, subject in ((True, valid), (False, invalid)):
                result.append({'id': f'{family}_{size}_' + ('accepted' if expected else 'malformed'),
                    'family': family, 'size': size, 'expected': expected, 'subject': subject,
                    'bytes': len(subject.encode('ascii')), 'scope': SCOPES[family],
                    'obligation': 'whole ASCII byte input recognition; no forest, semantic evaluation, or captures',
                    'provenance': provenance, 'construction': 'Frozen without engine timing or selected-case feedback.',
                    'mutation': None if expected else mutation, 'mutation_offset': None if expected else offset,
                    'planned_weight': 1/32})
    return result

def variants(case: dict, count: int):
    """Stable distinct same-length inputs, first variant retains original bytes."""
    family, base = case['family'], case['subject']
    marker = {'csv': 'case,', 'ini': 'PPI', 'xml_subset': 'alpha0000',
              'arithmetic_expression': 'var000'}[family]
    # The small CSV header and large header both start with case,.
    assert marker in base, (family, marker)
    for index in range(count):
        if index == 0:
            text = base
        else:
            if family == 'csv':
                replacement = 'v' + f'{index:03d}' + ','
            elif family == 'ini':
                replacement = chr(65 + index//26 % 26) + chr(65 + index%26) + 'I'
            elif family == 'xml_subset':
                replacement = 'alpha' + f'{index:04d}'
            else:
                replacement = 'var' + f'{index:03d}'
            text = base.replace(marker, replacement, 1)
        assert len(text) == len(base) and accepts(family, text) == case['expected'], (case['id'], index)
        yield text

def correctness_cases():
    edges = {
        'csv': {True: ['\n', ',\n', 'a,b\n', 'a,b\r\n', '"a,b","a""b"\n', '"a\nb",c\n', '"",\n'],
                False: ['', 'a,b', 'a"b,c\n', '"a,b\n', '"x" y\n', 'a\rb\n', '\t,c\n']},
        'ini': {True: ['\n', '; note\n', 'a=1\n', ' [main] ; note\r\n', ':version = 1.2\n', '-body=x;y\n'],
                False: ['', 'a=1', '[]\n', '[main\n', '[main]junk\n', 'a\n', '1a=x\n', 'a=\x00\n']},
        'xml_subset': {True: ['<root/>', ' <node/>\n', '<root a="x &amp; y"><item>42</item></root>', '<group><node><item/></node></group>', '<root a="1" a="2"/>'],
                False: ['', '<root>', '<root></node>', '<other/>', '<root a=\'1\'/>', '<root>&oops;</root>', '<root><item/></root><node/>', '<root>raw > byte</root>']},
        'arithmetic_expression': {True: ['1', 'a+b*c', '(a+b)*c', '- -a + 1.2', 'a/(b-1)', '_x + +2'],
                False: ['', 'a+', '()', 'a b', '1.', '.1', '1e3', '(a', 'a)', 'a**b', 'a=2']},
    }
    rows = []
    for family, outcomes in edges.items():
        for expected, texts in outcomes.items():
            for i, text in enumerate(texts):
                assert accepts(family, text) == expected, (family, expected, text)
                rows.append({'id': f'{family}_edge_{int(expected)}_{i}', 'family': family,
                    'expected': expected, 'subject': text, 'bytes': len(text)})
    return rows
