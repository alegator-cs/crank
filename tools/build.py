#!/usr/bin/env python3
"""Build declared Crank components sequentially, with checked dependency receipts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p, obj):
    p.parent.mkdir(parents=True, exist_ok=True)
    temp = p.with_suffix(p.suffix + '.tmp')
    temp.write_text(json.dumps(obj, indent=2) + '\n')
    temp.replace(p)
def build(target, args, manifest):
    cxx = shlex.split(os.environ.get('CXX', 'c++'))
    version = subprocess.check_output(cxx + ['--version'], text=True).splitlines()[0]
    flags = ['-std=c++20', '-pthread', '-Wall', '-Wextra', '-Wpedantic', '-Iengine', '-I.']
    flags += ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else ['-O2']
    if args.pcre2: flags += ['-DCRANK_WITH_PCRE2=1']
    flags += shlex.split(os.environ.get('EXTRA_CXXFLAGS', ''))
    mode = 'hybrid' + ('-san' if args.sanitize else '') + ('-pcre2' if args.pcre2 else '')
    folder = ROOT / 'build' / mode
    folder.mkdir(parents=True, exist_ok=True)
    sources = list(manifest['sources'])
    if target == 'libcrank': extra = []
    elif target in manifest['targets']: extra = [manifest['targets'][target]]
    else: raise SystemExit('Unknown target: ' + target)
    sources += manifest.get('target_sources', {}).get(target, [])
    objects, receipts = [], []
    for source in sources + extra:
        obj = folder / (source.replace('/', '_') + '.o')
        receipt = obj.with_suffix('.json')
        identity = {'source': source, 'flags': flags, 'compiler': cxx, 'version': version}
        old = json.loads(receipt.read_text()) if receipt.exists() else {}
        valid = obj.exists() and obj.stat().st_size > 0 and old.get('identity') == identity and old.get('object_sha256') == digest(obj)
        valid = valid and all((ROOT / p).exists() and digest(ROOT / p) == h for p, h in old.get('dependencies', {}).items()) and bool(old.get('dependencies'))
        if not valid:
            depfile = obj.with_suffix('.d')
            cmd = cxx + flags + ['-MMD', '-MF', str(depfile), '-c', source, '-o', str(obj)]
            print('compile', source, flush=True)
            subprocess.run(cmd, cwd=ROOT, check=True)
            if not obj.exists() or obj.stat().st_size == 0:
                raise SystemExit('Compiler produced no object bytes: ' + str(obj))
            # Compiler-generated make dependencies; shlex handles escaped spaces.
            deptext = depfile.read_text().replace('\\\n', ' ')
            paths = shlex.split(deptext.split(':', 1)[1])
            dependencies = {p: digest(ROOT / p) for p in paths}
            old = {'identity': identity, 'dependencies': dependencies, 'object_sha256': digest(obj), 'command': cmd}
            save(receipt, old)
        objects.append(obj); receipts.append(old)
    for obj, receipt in zip(objects, receipts):
        if not obj.exists() or obj.stat().st_size == 0 or digest(obj) != receipt['object_sha256'] or any(digest(ROOT / p) != h for p, h in receipt['dependencies'].items()):
            raise SystemExit('Input changed during build; rerun: ' + str(obj))
    output = folder / ('libcrank.a' if target == 'libcrank' else target)
    # ar updates members in place; removed translation units must not survive
    # from an earlier build graph.
    if target == 'libcrank': output.unlink(missing_ok=True)
    cmd = ['ar', 'rcs', str(output)] + list(map(str, objects)) if target == 'libcrank' else cxx + flags + list(map(str, objects)) + (['-lpcre2-8'] if args.pcre2 else []) + ['-o', str(output)]
    print('link', output.relative_to(ROOT), flush=True)
    subprocess.run(cmd, cwd=ROOT, check=True)
    if not output.exists() or output.stat().st_size == 0:
        raise SystemExit('Linker produced no output bytes: ' + str(output))
    save(folder / (target + '.build.json'), {'target': target, 'mode': mode, 'objects': receipts, 'link_command': cmd, 'binary_sha256': digest(output)})
    return output

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('targets', nargs='*')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--run', action='store_true', help='Run each requested executable after building it')
    parser.add_argument('--test', action='store_true', help='Build and run every declared C++ regression target')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--pcre2', action='store_true')
    args = parser.parse_args()
    manifest = json.loads((ROOT / 'build.json').read_text())
    if args.list:
        print('\n'.join(['libcrank'] + list(manifest['targets'])))
        return
    targets = manifest['tests'] if args.test else (args.targets or ['crank'])
    if (args.test or args.run) and 'libcrank' in targets: parser.error('Cannot run a static library')
    for target in targets:
        binary = build(target, args, manifest)
        if args.test or args.run: subprocess.run([str(binary)], cwd=ROOT, check=True)
if __name__ == '__main__': main()
