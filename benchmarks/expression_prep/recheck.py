"""Targeted, separately reported follow-up of historical regressions and a control."""
import argparse,hashlib,json,pathlib,statistics,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
default_bins=ROOT/'build/expression_prep/paired/binaries'
parser.add_argument('--baseline-binary',type=pathlib.Path,default=default_bins/'baseline/stage_runner')
parser.add_argument('--modified-binary',type=pathlib.Path,default=default_bins/'modified/stage_runner')
parser.add_argument('--out',type=pathlib.Path,default=ROOT/'build/expression_prep/recheck')
parser.add_argument('--pairs',type=int,default=16)
args=parser.parse_args()
if not 1<=args.pairs<=100:parser.error('pairs must be 1..100')
bins={'baseline':args.baseline_binary.resolve(),'modified':args.modified_binary.resolve()}
for binary in bins.values():
    if not binary.is_file():parser.error('binary does not exist: '+str(binary))
OUT=args.out.resolve()
if OUT.exists() and any(OUT.iterdir()):
    parser.error('output directory is not empty; choose a new --out directory')
OUT.mkdir(parents=True,exist_ok=True)
targets=[('shared_count','parse_source',100000),('shared_count','program_integer_on',30000),
    ('affine','program_integer_on',4000),('identifier','root_regular_fragment',10000),
    ('identifier','regular_eligibility',100000),('nested_groups_64','program_integer_on',3000)]
samples=[]
for target,(fixture,stage,reps) in enumerate(targets):
    for pair in range(args.pairs):
        order=['baseline','modified'] if pair%2==0 else ['modified','baseline'];both={}
        for mode in order:
            binary=bins[mode]
            command=[str(binary),'--fixture',fixture,'--stage',stage,'--reps',str(reps),'--rounds','1','--max-cpu-ms','1000']
            name=f'{target:02d}-{pair:02d}-{mode}'
            with (OUT/(name+'.jsonl')).open('w') as out,(OUT/(name+'.stderr')).open('w') as err:
                r=subprocess.run(command,cwd=ROOT,stdout=out,stderr=err,timeout=30)
            if r.returncode:raise RuntimeError(name+' failed')
            rows=[json.loads(s) for s in (OUT/(name+'.jsonl')).read_text().splitlines()]
            values=[r for r in rows if r['kind']=='timing']
            if len(values)!=1 or rows[-1]['kind']!='complete':raise RuntimeError('invalid target output')
            both[mode]=values[0]
        a,b=both['baseline'],both['modified']
        if (a['reps'],a['checksum'])!=(b['reps'],b['checksum']):raise RuntimeError('target checksum differs')
        samples.append({'fixture':fixture,'stage':stage,'pair':pair,'order':order,'reps':reps,
            'baseline_cpu_ns':a['cpu_ns'],'modified_cpu_ns':b['cpu_ns'],
            'baseline_wall_ns':a['wall_ns'],'modified_wall_ns':b['wall_ns']})
    print('completed',fixture,stage,flush=True)
rows=[]
for fixture,stage,reps in targets:
    ss=[s for s in samples if (s['fixture'],s['stage'])==(fixture,stage)]
    ratios=[s['baseline_cpu_ns']/s['modified_cpu_ns'] for s in ss]
    rows.append({'fixture':fixture,'stage':stage,'reps':reps,'pairs':len(ss),
        'baseline_us':statistics.median(s['baseline_cpu_ns']/reps/1000 for s in ss),
        'modified_us':statistics.median(s['modified_cpu_ns']/reps/1000 for s in ss),
        'paired_speedup_median':statistics.median(ratios),'paired_speedup_min':min(ratios),'paired_speedup_max':max(ratios),
        'pairs_faster':sum(r>1 for r in ratios)})
(OUT/'pairs.json').write_text(json.dumps(samples,indent=2)+'\n')
(OUT/'summary.json').write_text(json.dumps({'reason':'nominal primary regressions and positive control; separate results; no primary rows replaced',
    'pairs':args.pairs,'timing_uses_standard_allocator':True,
    'binaries':{mode:{'sha256':hashlib.sha256(binary.read_bytes()).hexdigest()} for mode,binary in bins.items()},
    'rows':rows},indent=2)+'\n')
print(json.dumps(rows,indent=2),flush=True)
