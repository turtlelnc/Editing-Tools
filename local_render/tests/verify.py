"""Full-render oracle, local-range differential tests, and effect activation controls."""
import sys,copy,json,hashlib,random,math,struct,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import adapter
P=adapter.P;R=P/'results';t=json.loads((P/'fixture.json').read_text())

def pcm(b):return struct.unpack('<'+'f'*(len(b)//4),b)
def sha(b):return hashlib.sha256(b).hexdigest()
def run_ref(name,timeline):
    print('reference',name,flush=True)
    fs=list(adapter.render(timeline,0,timeline['frames'],R/name))
    meta=[{k:v for k,v in f.items() if k not in ['video','audio']}|{'video_sha256':sha(f['video']),'audio_sha256':sha(f['audio'])} for f in fs]
    (R/name/'frames.json').write_text(json.dumps(meta,indent=2));return fs

def compare(ref,timeline,begin,end,policy,warmup,name):
    begin_time=time.perf_counter();bad_v=[];bad_a=[];bad_time=[];maxaudio=0;sum2=0;n=0;out_n=0;bad_samples=0;maxpixel=0;pixel_count=0
    for f in adapter.render(timeline,begin,end,R/name,policy,warmup):
        i=f['frame'];expect=ref[i];out_n+=1
        if f['video']!=expect['video']:
            bad_v.append(i)
            diffs=[abs(a-b) for a,b in zip(f['video'],expect['video'])];maxpixel=max(maxpixel,max(diffs));pixel_count+=sum(d>0 for d in diffs)
        if f['audio']!=expect['audio']:
            bad_a.append(i);a=pcm(f['audio']);b=pcm(expect['audio']);ds=[x-y for x,y in zip(a,b)];maxaudio=max(maxaudio,max((abs(d) for d in ds),default=0));sum2+=sum(d*d for d in ds);n+=len(ds);bad_samples+=sum(d!=0 for d in ds)
        for k in ['frame','mlt_position','sample_start','samples']:
            if f[k]!=expect[k]:bad_time.append([i,k,f[k],expect[k]])
    x={'range':[begin,end],'policy':policy,'warmup':warmup,'frames':out_n,'video_mismatch_frames':bad_v,'audio_mismatch_frames':bad_a,'audio_different_channel_samples':bad_samples,'timestamp_mismatches':bad_time,'max_pixel_byte_error':maxpixel,'different_pixel_bytes':pixel_count,'max_audio_abs_error':maxaudio,'audio_rms_error_on_mismatch_frames':math.sqrt(sum2/n) if n else 0,'seconds':time.perf_counter()-begin_time,'exact':not bad_v and not bad_a and not bad_time and out_n==end-begin}
    print(name, 'PASS' if x['exact'] else f"video={len(bad_v)} audio={len(bad_a)}",flush=True)
    return x

rng=random.Random(20261014);ranges=[(1,9),(30,41),(58,66),(72,93),(84,90),(129,136),(144,154),(159,174)]
ranges += [(s:=rng.randrange(5,167),min(180,s+rng.randrange(3,16))) for _ in range(6)]
cases={
 'stateless':{**t,'effects':[]},
 'temporal':{**t,'effects':[t['effects'][0]]},
 'reverb':{**t,'effects':[t['effects'][1]]},
 'combined':t,
 'stacked':{**t,'effects':[t['effects'][0],{**t['effects'][0],'id':'tmix_second','frames':3}]}}
refs={name:run_ref('ref-'+name,c) for name,c in cases.items()}
rows=[]
for case in ['stateless','temporal','combined']:
    for j,(begin,end) in enumerate(ranges):
        policies=['cold'] if case=='stateless' else ['cold','bounded','exact']
        for policy in policies:
            rows.append({'case':case,**compare(refs[case],cases[case],begin,end,policy,0,f'{case}-{policy}-{j}')})
for j,(begin,end) in enumerate([(72,85),(129,136),(159,174)]):
    for policy,warm in [('cold',0),('fixed',30),('fixed',90),('exact',0)]:
        rows.append({'case':'reverb',**compare(refs['reverb'],cases['reverb'],begin,end,policy,warm,f'reverb-{policy}-{warm}-{j}')})
for warm in [0,5,6]:
    rows.append({'case':'stacked',**compare(refs['stacked'],cases['stacked'],73,92,'fixed',warm,f'stacked-{warm}')})
# Fault injection: claimed four-frame history reduced to three must be detectable.
rows.append({'case':'temporal',**compare(refs['temporal'],cases['temporal'],73,92,'fixed',3,'history-mutation')})
# Deterministic fresh-state controls: repeat whole graph and replay anchor must agree.
rows.append({'case':'combined',**compare(refs['combined'],t,0,180,'exact',0,'repeat-full')})
# NTSC audio packet sizes are position-dependent; verify exact replay and rational endpoints.
ntsc=copy.deepcopy(t);ntsc['fps']=[30000,1001];ntsc_ref=run_ref('ref-ntsc',ntsc)
for j,(begin,end) in enumerate([(58,66),(129,136),(159,174)]):
    rows.append({'case':'ntsc',**compare(ntsc_ref,ntsc,begin,end,'exact',0,f'ntsc-{j}')})
ntsc_boundaries=[]
for f in ntsc_ref:
    expected=(f['frame']*48000*1001*2+30000)//(2*30000)
    if f['sample_start']!=expected:ntsc_boundaries.append([f['frame'],f['sample_start'],expected])
# Ablation proves every requested effect is active, not merely listed in XML.
controls=[]
for feature in ['crossfade','speed','overlay','subtitle','tmix','reverb']:
    c=copy.deepcopy(t)
    if feature=='crossfade':c['crossfade']['disabled']=True
    elif feature=='speed':next(x for x in c['clips'] if x['id']=='b')['speed']=[1,1]
    elif feature=='overlay':c['clips']=[x for x in c['clips'] if x['id']!='overlay']
    elif feature=='subtitle':c['clips']=[x for x in c['clips'] if x['kind']!='subtitle']
    elif feature=='tmix':c['effects']=[x for x in c['effects'] if x['type']!='tmix']
    elif feature=='reverb':c['effects']=[x for x in c['effects'] if x['type']!='sox.reverb']
    other=run_ref('ablation-'+feature,c)
    v=[i for i,(a,b) in enumerate(zip(refs['combined'],other)) if a['video']!=b['video']]
    a=[i for i,(a,b) in enumerate(zip(refs['combined'],other)) if a['audio']!=b['audio']]
    controls.append({'feature':feature,'video_changed_frames':v,'audio_changed_frames':a,'active':bool(v or a)})
# Reverb is deliberately allowed to run on silence after the source tone stops.
tail=pcm(b''.join(f['audio'] for f in refs['reverb'][120:180]));dry=pcm(b''.join(f['audio'] for f in refs['stateless'][120:180]))
result={'seed':20261014,'ranges':ranges,'cases':rows,'activation_controls':controls,'ntsc_sample_start_mismatches':ntsc_boundaries,'tail_rms':math.sqrt(sum(x*x for x in tail)/len(tail)),'dry_tail_rms':math.sqrt(sum(x*x for x in dry)/len(dry)),
 'strict_failures':[r for r in rows if (r['policy']=='exact' or r['case']=='stateless' or r['case']=='temporal' and r['policy']=='bounded' or r['case']=='stacked' and r['warmup']==6) and not r['exact']]}
(R/'verification.json').write_text(json.dumps(result,indent=2))
print('strict failures',len(result['strict_failures']),'activation',[x['active'] for x in controls],flush=True)
# Expected cold/fixed mismatches are measurements; only strict contract failures fail the run.
if result['strict_failures'] or not all(x['active'] for x in controls) or ntsc_boundaries or result['tail_rms']<=1e-5 or result['dry_tail_rms']!=0:sys.exit(1)
