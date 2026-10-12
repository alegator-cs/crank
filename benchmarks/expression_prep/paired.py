"""Serial AB/BA fresh-preparation CPU measurements; run under nice/taskset."""
import datetime, hashlib, json, math, os, pathlib, statistics, subprocess, time
ROOT=pathlib.Path(__file__).resolve().parents[2]
OUT=ROOT.parent/'evidence'/'paired-preparation'
OUT.mkdir(parents=True,exist_ok=True)
SOURCE=pathlib.Path(__file__).with_name('stage_runner.cpp')
bins={mode:ROOT/'build'/('expression-'+mode)/'stage_runner' for mode in ['baseline','modified']}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def run(mode,name,extra,allocation_binary=False):
    path=OUT/(name+'.jsonl')
    binary=bins[mode].with_name('stage_runner_alloc') if allocation_binary else bins[mode]
    cmd=[str(binary),'--rounds','1','--max-cpu-ms','60000',*extra]
    start=time.monotonic()
    with path.open('w') as out,(OUT/(name+'.stderr')).open('w') as err:
        result=subprocess.run(cmd,cwd=ROOT,stdout=out,stderr=err,timeout=240)
    if result.returncode:raise RuntimeError(f'{name} failed: '+(OUT/(name+'.stderr')).read_text())
    rows=[json.loads(line) for line in path.read_text().splitlines()]
    if not rows or rows[-1]['kind']!='complete':raise RuntimeError('incomplete run '+name)
    print(name,'wall_seconds_operational',round(time.monotonic()-start,3),flush=True)
    return rows
manifest={'source_sha256':sha(SOURCE),'compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
    'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'flags':['-std=c++20','-O2','-g0','-pthread','-Iengine','-I.'],'pairs':8,'target_batch_cpu_ms':10,
    'max_reps':100000,'max_cpu_ms_per_run':60000,'child_wall_cap_seconds':240,
    'clock':'CLOCK_PROCESS_CPUTIME_ID','affinity':sorted(os.sched_getaffinity(0)),
    'nice':os.getpriority(os.PRIO_PROCESS,0),'scope':'grammar-source preparation, fresh objects; destruction included; components overlap',
    'no_GPU':True,'no_installs':True,'quiet_wall_latency_claim':False,
    'timing_uses_standard_allocator':True,'binaries':{}}
for mode,binary in bins.items():
    print('compile harness',mode,flush=True)
    lib=binary.parent/'libcrank.a'
    subprocess.run(['g++',*manifest['flags'],str(SOURCE),str(lib),'-o',str(binary)],cwd=ROOT,check=True,timeout=180)
    alloc_binary=binary.with_name('stage_runner_alloc')
    subprocess.run(['g++',*manifest['flags'],'-DCRANK_EXPRESSION_ALLOCATIONS',str(SOURCE),str(lib),'-o',str(alloc_binary)],cwd=ROOT,check=True,timeout=180)
    manifest['binaries'][mode]={'sha256':sha(binary),'allocation_binary_sha256':sha(alloc_binary),'library_sha256':sha(lib)}
cal={mode:run(mode,'calibration-'+mode,['--reps','4']) for mode in bins}
allocation_runs={mode:run(mode,'allocation-'+mode,['--reps','1','--allocations'],True) for mode in bins}
timing=lambda rows:{(r['fixture'],r['stage']):r for r in rows if r['kind']=='timing'}
base=timing(cal['baseline']);other=timing(cal['modified'])
if set(base)!=set(other):raise RuntimeError('different completed stage sets')
identity=lambda rows:[r for r in rows if r['kind'] in ['fixture','diagnostic','declined']]
if identity(cal['baseline'])!=identity(cal['modified']):raise RuntimeError('fixture, diagnostic or decline differs')
plan=OUT/'repetitions.tsv'
with plan.open('w') as out:
    for key,r in base.items():
        reps=max(4,min(100000,math.ceil(10_000_000/(r['cpu_ns']/r['reps']))))
        out.write(' '.join([*key,str(reps)])+'\n')
samples=[]
for pair in range(8):
    order=['baseline','modified'] if pair%2==0 else ['modified','baseline']
    paired={}
    for mode in order:
        rows=run(mode,f'pair-{pair:02d}-{mode}',['--reps','4','--plan',str(plan)])
        if identity(rows)!=identity(cal[mode]):raise RuntimeError('run identity differs')
        paired[mode]=timing(rows)
    if set(paired['baseline'])!=set(base) or set(paired['modified'])!=set(base):raise RuntimeError('stage set differs')
    for key in base:
        a,b=(paired[m][key] for m in ['baseline','modified'])
        if (a['reps'],a['checksum'])!=(b['reps'],b['checksum']):raise RuntimeError('paired preparation result differs')
        samples.append({'pair':pair,'order':order,'fixture':key[0],'stage':key[1],
            'reps':a['reps'],'baseline_cpu_ns':a['cpu_ns'],'modified_cpu_ns':b['cpu_ns'],
            'baseline_wall_ns':a['wall_ns'],'modified_wall_ns':b['wall_ns']})
def family(f):
    if f.startswith('bad_'):return 'malformed'
    if f in ['shared_count','backreference','affine']:return 'stateful'
    if f in ['balanced_nullable','mutual_nullable','acyclic_calls','json_source','recursive_alternatives_32','recursive_count']:return 'recursive_or_called'
    if f in ['literal_64','literal_1024','nested_groups_64','alternatives_128','concat_256']:return 'larger_regular'
    return 'small_common_regular'
alloc={mode:{(r['fixture'],r['stage']):r for r in allocation_runs[mode] if r['kind']=='allocation'} for mode in bins}
rows=[]
for key in base:
    ss=[r for r in samples if (r['fixture'],r['stage'])==key]
    aa=[r['baseline_cpu_ns']/r['reps']/1000 for r in ss];bb=[r['modified_cpu_ns']/r['reps']/1000 for r in ss]
    ratios=[a/b for a,b in zip(aa,bb)]
    row={'fixture':key[0],'family':family(key[0]),'stage':key[1],
        'baseline_us':statistics.median(aa),'modified_us':statistics.median(bb),
        'paired_speedup_median':statistics.median(ratios),'paired_speedup_min':min(ratios),'paired_speedup_max':max(ratios),
        'baseline_min_batch_cpu_us':min(r['baseline_cpu_ns'] for r in ss)/1000,
        'modified_min_batch_cpu_us':min(r['modified_cpu_ns'] for r in ss)/1000,
        'reps':ss[0]['reps'],'baseline_allocations':alloc['baseline'].get(key),'modified_allocations':alloc['modified'].get(key)}
    rows.append(row)
gm=lambda xs:math.exp(statistics.mean(math.log(x) for x in xs))
aggregates={}
for stage in sorted({r['stage'] for r in rows}):
    group=[r for r in rows if r['stage']==stage]
    families={f:gm([r['paired_speedup_median'] for r in group if r['family']==f]) for f in sorted({r['family'] for r in group})}
    aggregates[stage]={'equal_case_geomean_speedup':gm([r['paired_speedup_median'] for r in group]),
        'equal_family_geomean_speedup':gm(list(families.values())),'families':families,
        'cases':len(group),'case_speedup_min':min(r['paired_speedup_median'] for r in group),
        'case_speedup_max':max(r['paired_speedup_median'] for r in group)}
summary={'manifest':manifest,'raw_timing_rows':len(samples)*2,'stage_pairs':len(samples),
    'calibration_identity_equal':True,'paired_checksums_equal':True,
    'declined': [r for r in cal['baseline'] if r['kind']=='declined'],
    'aggregates':aggregates,'rows':rows}
(OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(OUT/'pairs.json').write_text(json.dumps(samples,indent=2)+'\n')
(OUT/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({k:v for k,v in aggregates.items() if k in ['parse_source','program_integer_on','program_integer_off','root_regular_fragment']},indent=2),flush=True)
