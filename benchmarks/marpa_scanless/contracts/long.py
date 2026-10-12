"""Iterative independent oracles and synthetic large-fixture construction."""
from __future__ import annotations
import csv, datetime, hashlib, io, json, re, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PRIOR = ROOT.parent
FAMILIES = ['regex_syntax', 'nested_comma_list', 'event_stream', 'csv', 'ini',
            'xml_subset', 'arithmetic_expression']
LOWER = 'abcdefghijklmnopqrstuvwxyz'
DIGITS = '0123456789'
WHITE = ' \t\r\n'
ENTITIES = ('&amp;', '&lt;', '&gt;', '&quot;', '&apos;')
TAGS = ('root', 'node', 'item', 'group')

def sha(raw): return hashlib.sha256(raw).hexdigest()
def utc(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def write_json(p, v): p.write_text(json.dumps(v, indent=2) + '\n', encoding='utf8')

class Invalid(ValueError): pass
class Cursor:
    def __init__(self, text): self.text, self.pos = text, 0
    def take(self, literal):
        if self.text.startswith(literal, self.pos):
            self.pos += len(literal); return True
        return False
    def need(self, literal):
        if not self.take(literal): raise Invalid(literal)
    def run(self, chars, minimum=0):
        start = self.pos
        while self.pos < len(self.text) and self.text[self.pos] in chars: self.pos += 1
        if self.pos - start < minimum: raise Invalid('missing run')

def regex_accepts(text):
    depth, need_atom, allow_postfix = 0, True, False
    for c in text:
        if need_atom:
            if c in LOWER: need_atom, allow_postfix = False, True
            elif c == '(': depth += 1; allow_postfix = False
            else: return False
        elif c in LOWER: allow_postfix = True
        elif c == '(': depth += 1; need_atom, allow_postfix = True, False
        elif c in '*+?' and allow_postfix: allow_postfix = False
        elif c == '|': need_atom, allow_postfix = True, False
        elif c == ')' and depth: depth -= 1; allow_postfix = True
        else: return False
    return not need_atom and depth == 0

def list_accepts(text):
    p = Cursor(text); depth, need_item = 0, True
    try:
        while p.pos < len(text):
            if need_item:
                if p.take('('): depth += 1
                else: p.run(LOWER, 1); p.run(LOWER + DIGITS); need_item = False
            elif p.take(','): need_item = True
            elif p.take(')') and depth: depth -= 1
            else: return False
        return not need_item and depth == 0
    except Invalid: return False

def event_accepts(text):
    p = Cursor(text); records = 0
    try:
        while p.pos < len(text):
            stack = ['event']
            while stack:
                state = stack.pop()
                if state == 'event': p.need('E['); stack.extend(('event_tail', 'field'))
                elif state == 'field':
                    p.run(LOWER, 1); p.run(LOWER + DIGITS + '_'); p.need('='); stack.append('value')
                elif state == 'event_tail':
                    if p.take(';'): stack.extend(('event_tail', 'field'))
                    else: p.need(']')
                elif state == 'list_tail':
                    if p.take(','): stack.extend(('list_tail', 'value'))
                    else: p.need(']')
                elif state == 'value':
                    if p.text.startswith('E[', p.pos): stack.append('event')
                    elif p.take('['):
                        if not p.take(']'): stack.extend(('list_tail', 'value'))
                    elif p.take('"'):
                        p.run('ABCDEFGHIJKLMNOPQRSTUVWXYZ' + LOWER + DIGITS + ' _;=/-'); p.need('"')
                    else: p.run(DIGITS, 1)
                else: raise AssertionError(state)
            p.need('\n'); records += 1
        return records > 0
    except Invalid: return False

def csv_accepts(text):
    i, records, n = 0, 0, len(text)
    while i < n:
        while True:
            if i < n and text[i] == '"':
                i += 1
                while i < n:
                    if text[i] == '"':
                        if i+1 < n and text[i+1] == '"': i += 2; continue
                        i += 1; break
                    if text[i] not in '\t\r\n' and not 32 <= ord(text[i]) <= 126: return False
                    i += 1
                else: return False
            else:
                while i < n and text[i] not in ',\r\n':
                    if text[i] == '"' or not 32 <= ord(text[i]) <= 126: return False
                    i += 1
            if i < n and text[i] == ',': i += 1; continue
            if i < n and text[i] == '\r': i += 1
            if i >= n or text[i] != '\n': return False
            i += 1; records += 1; break
    return records > 0

INI_KEY = re.compile(r'[A-Za-z_:-][A-Za-z0-9_.:/ \t-]*')
INI_SECTION = re.compile(r'[A-Za-z0-9_.:/ @-]+')
def ini_accepts(text):
    if not text or not text.endswith('\n'): return False
    for line in text.split('\n')[:-1]:
        if line.endswith('\r'): line = line[:-1]
        if any(c != '\t' and not 32 <= ord(c) <= 126 for c in line): return False
        line = line.lstrip(' \t')
        if not line or line[0] in ';#': continue
        if line[0] == '[':
            if ']' not in line: return False
            name, tail = line[1:].split(']', 1)
            if not INI_SECTION.fullmatch(name): return False
            tail = tail.lstrip(' \t')
            if tail and tail[0] not in ';#': return False
        else:
            key, sep, value = line.partition('=')
            if not sep or not INI_KEY.fullmatch(key): return False
    return True

XML_NAME = re.compile(r'[A-Za-z_][A-Za-z0-9_.:-]*')
def xml_accepts(text):
    """Iterative finite-vocabulary XML recognizer, with exact source alphabets."""
    p = Cursor(text); stack = []; root_seen = False
    try:
        p.run(WHITE)
        while p.pos < len(text):
            if p.text.startswith('</', p.pos):
                p.need('</')
                if not stack: return False
                p.need(stack.pop()); p.need('>')
                if not stack: p.run(WHITE); return p.pos == len(text)
            elif p.take('<'):
                if root_seen and not stack: return False
                tag = next((t for t in TAGS if text.startswith(t, p.pos)), None)
                if tag is None: return False
                p.need(tag); root_seen = True
                closed = False
                while True:
                    old = p.pos; p.run(WHITE); had_white = p.pos > old
                    if p.take('/>'): closed = True; break
                    if p.take('>'): break
                    if not had_white: return False
                    m = XML_NAME.match(text, p.pos)
                    if not m: return False
                    p.pos = m.end(); p.run(WHITE); p.need('='); p.run(WHITE); p.need('"')
                    while not p.take('"'):
                        if p.pos >= len(text): return False
                        c = text[p.pos]
                        if c == '&':
                            entity = next((v for v in ENTITIES if text.startswith(v, p.pos)), None)
                            if entity is None: return False
                            p.need(entity)
                        elif 32 <= ord(c) <= 126 and c not in '<"': p.pos += 1
                        else: return False
                if closed:
                    if not stack: p.run(WHITE); return p.pos == len(text)
                else: stack.append(tag)
            elif stack:
                c = text[p.pos]
                if c == '&':
                    entity = next((v for v in ENTITIES if text.startswith(v, p.pos)), None)
                    if entity is None: return False
                    p.need(entity)
                elif c in '\t\r\n' or 32 <= ord(c) <= 126 and c not in '<>': p.pos += 1
                else: return False
            else: return False
        return False
    except Invalid: return False

ARITH_TOKEN = re.compile(r'(?:[0-9]+(?:\.[0-9]+)?|[A-Za-z_][A-Za-z0-9_]*|[()+*/-])')
def arithmetic_accepts(text):
    """Iterative token syntax; precedence changes tree shape, never membership."""
    at, depth, need_operand = 0, 0, True
    while at < len(text):
        if text[at] in WHITE: at += 1; continue
        m = ARITH_TOKEN.match(text, at)
        if not m: return False
        token = m.group(); at = m.end()
        if need_operand:
            if token in ('+', '-'): continue
            if token == '(': depth += 1; continue
            if token in (')', '*', '/'): return False
            need_operand = False
        else:
            if token in ('+', '-', '*', '/'): need_operand = True
            elif token == ')' and depth: depth -= 1
            else: return False
    return not need_operand and depth == 0

ORACLES = dict(regex_syntax=regex_accepts, nested_comma_list=list_accepts,
               event_stream=event_accepts, csv=csv_accepts, ini=ini_accepts,
               xml_subset=xml_accepts, arithmetic_expression=arithmetic_accepts)
def accepts(family, subject): return subject.isascii() and ORACLES[family](subject)
def escape_xml(s): return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')

def positive(family, target, depth, csv_seed, ini_seed):
    if family == 'regex_syntax':
        prefix, row = '(alpha|beta)*', '(gamma|delta)?'
        n = max(1, (target-len(prefix)-5)//len(row))
        subject = prefix + row*n + 'omega'
        subject += 'a' * (target-len(subject))
        return subject, {'construction': 'Bounded 256/1024-byte regex syntax; concatenated optional groups and a final literal run; binary grammar associations retained.', 'nesting_depth': 1, 'provenance': 'synthetic_practical_shaped_not_real_world'}
    if family == 'nested_comma_list':
        prefix = '(' + '('*(depth-1) + 'alpha,beta' + ')'*(depth-1)
        rows = []; size = len(prefix)+1; i = 0
        while size < target:
            row = ',(name%d,(value%d,delta),tail%d)' % (i,i,i)
            rows.append(row); size += len(row); i += 1
        return prefix+''.join(rows)+')', {'construction': 'One deep nested list plus a long sequence of varied identifier groups.', 'nesting_depth': depth, 'records': i, 'provenance': 'synthetic_practical_shaped_not_real_world'}
    if family == 'event_stream':
        prefix = 'E[msg="route";child=' + 'E[value='*(depth-1) + '0' + ']'*(depth-1) + ';samples=[]]\n'
        rows = []; size = len(prefix); i = 0
        while size < target:
            if i%3 == 0: row = 'E[id=%d;msg="alpha beta";samples=[1,2,3];child=E[ok=1]]\n' % i
            elif i%3 == 1: row = 'E[id=%d;flags=[];samples=[[],E[value=2],"ok"]]\n' % i
            else: row = 'E[id=%d;msg="route ready";child=E[ok=1;more=E[value=2]]]\n' % i
            rows.append(row); size += len(row); i += 1
        return prefix+''.join(rows), {'construction': 'LF event log with one deep child chain and varied subsequent numeric/string/list/nested-event records.', 'nesting_depth': depth, 'records': i+1, 'provenance': 'synthetic_practical_shaped_not_real_world'}
    if family == 'csv':
        prefix = 'alpha,context,"comma,quote ""escaped""",,\r\n'
        n = max(1, (target-len(prefix)+len(csv_seed)-1)//len(csv_seed))
        return prefix+csv_seed*n, {'construction': 'Synthetic CSV seed concatenated repeatedly, preceded by one explicit synthetic quoted/empty-field marker record.', 'nesting_depth': 0, 'source_repetitions': n, 'provenance': 'synthetic_public_fixture'}
    if family == 'ini':
        prefix = 'benchmark_marker=alpha\n'
        n = max(1, (target-len(prefix)+len(ini_seed)-1)//len(ini_seed))
        return prefix+ini_seed*n, {'construction': 'Synthetic INI seed concatenated repeatedly, preceded by one explicit synthetic marker assignment.', 'nesting_depth': 0, 'source_repetitions': n, 'provenance': 'synthetic_public_fixture'}
    if family == 'xml_subset':
        values = list(csv.reader(io.StringIO(csv_seed)))
        text_rows = [' | '.join(escape_xml(v) for v in row) for row in values]
        prefix = '<root marker="alpha">\n' + '<group>'*(depth-1) + '<item>nested &amp; leaf</item>' + '</group>'*(depth-1) + '\n'
        rows = []; size = len(prefix)+len('</root>\n'); i = 0
        while size < target:
            row = '<node index="%d"><item>%s</item><item/></node>\n' % (i,text_rows[i%len(text_rows)])
            rows.append(row); size += len(row); i += 1
        return prefix+''.join(rows)+'</root>\n', {'construction': 'Synthetic fixed-vocabulary XML containers and nesting; text fields derive from synthetic CSV cells with XML escaping.', 'nesting_depth': depth+1, 'records': i, 'provenance': 'synthetic_public_fixture'}
    if family == 'arithmetic_expression':
        prefix = 'alpha + ' + '('*(depth-1) + 'nested_value' + ')'*(depth-1)
        rows = []; size = len(prefix)+1; i = 0
        while size < target:
            row = ' + ((base%d + 12.5) * gain_%d - -offset%d) / (scale_%d + 4)' % (i,i,i,i)
            rows.append(row); size += len(row); i += 1
        return prefix+''.join(rows)+'\n', {'construction': 'Long arithmetic expression with varied variables, decimals, unary signs, precedence, and one deeply parenthesized operand.', 'nesting_depth': depth-1, 'terms': i+2, 'provenance': 'synthetic_practical_shaped_not_real_world'}
    raise AssertionError(family)

def candidates(family, text, marker_pos):
    """Structural same-length edits, selected by offset rather than performance."""
    protected = range(marker_pos, marker_pos+5)
    if family == 'regex_syntax': return [(i,')','Replace literal with closing parenthesis') for i,c in enumerate(text) if c in LOWER and i not in protected]
    if family == 'nested_comma_list': return [(i,';','Replace comma with forbidden semicolon') for i,c in enumerate(text) if c == ',']
    if family == 'event_stream': return [(i,'(','Replace opening bracket with opening parenthesis') for i,c in enumerate(text) if c == '[']
    if family == 'csv':
        result = []; quoted = False; field_start = True; i = 0
        while i < len(text):
            c = text[i]
            if quoted:
                if c == '"':
                    if i+1 < len(text) and text[i+1]=='"': i += 2; continue
                    quoted = False
            elif c == '"' and field_start: quoted = True
            elif c in ',\r\n': field_start = True; i += 1; continue
            elif not field_start and i not in protected: result.append((i,'"','Insert double quote inside unquoted field by replacing one byte'))
            field_start = False; i += 1
        return result
    if family == 'ini':
        result = []; offset = 0
        for line in text.splitlines(keepends=True):
            stripped = line.lstrip(' \t')
            if stripped and stripped[0] not in '[;#\r\n' and '=' in line:
                result.append((offset+line.index('='),'[','Replace assignment delimiter with opening square bracket'))
            offset += len(line)
        return result
    if family == 'xml_subset': return [(m.start()+2,'X','Replace closing-tag name byte with X, mismatching fixed vocabulary') for m in re.finditer(r'</(?:root|node|item|group)>',text)]
    if family == 'arithmetic_expression': return [(i,'%','Replace multiplication with unsupported modulo operator') for i,c in enumerate(text) if c == '*']
    raise AssertionError(family)

def malformed(family, good, fraction, marker_pos):
    options = candidates(family,good,marker_pos)
    options.sort(key=lambda x:abs(x[0]-fraction*len(good)))
    # Membership, never speed, determines whether a structural edit is malformed.
    for offset, replacement, description in options[:12]:
        bad = good[:offset]+replacement+good[offset+1:]
        if not accepts(family,bad): return bad, offset, description
    raise AssertionError(('no guaranteed malformed structural edit',family,fraction))

def variant(case, index):
    if index == 0: return case['subject']
    value = index-1; suffix = []
    for _ in range(4): suffix.append(chr(97+value%26)); value //= 26
    replacement = 'v'+''.join(reversed(suffix))
    p = case['fresh_marker_offset']; text = case['subject']
    return text[:p]+replacement+text[p+5:]
