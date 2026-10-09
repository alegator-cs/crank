#!/usr/bin/env python3
"""Run the canonical C++ regressions and public CLI contracts once each."""
import argparse, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sanitize',action='store_true');p.add_argument('--pcre2',action='store_true');p.add_argument('--quick',action='store_true')
a=p.parse_args();flags=[x for x,b in [('--sanitize',a.sanitize),('--pcre2',a.pcre2)] if b]
def run(cmd): print(' '.join(cmd),flush=True);subprocess.run(cmd,cwd=ROOT,check=True)
run(['python3','tools/build.py','--test']+flags)
run(['python3','tools/build.py','crank','crank_reference','crank_assisted']+flags)
mode='hybrid'+('-san' if a.sanitize else '')+('-pcre2' if a.pcre2 else '')
binary=str(ROOT/'build'/mode/'crank')
run(['python3','tests/test_engine.py',binary]+(['--quick'] if a.quick else []))
run(['python3','tests/test_occurrence_cli.py',binary])
run(['python3','tests/test_capability_cli.py',binary]+(['--pcre2'] if a.pcre2 else []))
run(['python3','tests/test_assisted_cli.py',str(ROOT/'build'/mode/'crank_assisted')])
run(['python3','reference/test_differential.py',str(ROOT/'build'/mode/'crank_reference')]+(['--quick'] if a.quick else []))
