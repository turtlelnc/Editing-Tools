"""Default split versus independent legacy oracle; no raw media written."""
import copy,hashlib,json,sys
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter
P=adapter.P;R=P/'results/split-adapter';t=json.loads((P/'fixture.json').read_text());rows=[]
def signature(f):
 return {k:v for k,v in f.items() if k not in ('video','audio','source_position')}|{k:hashlib.sha256(f[k]).hexdigest() for k in ('video','audio')}
cases={'combined':t,'stateless':{**t,'effects':[]},'stacked':{**t,'effects':[t['effects'][0],{**t['effects'][0],'id':'second','frames':3}]},'no-padding':{**t,'clips':[c for c in t['clips'] if c['id']!='tail_padding']},'no-audio':{**t,'clips':[c for c in t['clips'] if c['kind']!='audio']},'ntsc':{**t,'fps':[30000,1001]}}
for name,c in cases.items():
 ref={f['frame']:signature(f) for f in adapter.render(c,0,180,R/name/'oracle',strategy='legacy')}
 for start,end in [(0,8),(58,93),(129,136),(159,180)]:
  out=[signature(f) for f in adapter.render(c,start,end,R/name/f'{start}-{end}')]
  assert out==[ref[i] for i in range(start,end)],(name,start)
  rows.append({'case':name,'range':[start,end],'frames':len(out),'exact':True})
# Abandoning a stream must release BOTH children, including the blocked pipe.
real=adapter.subprocess.Popen;children=[]
def spawn(*a,**kw):
 p=real(*a,**kw);children.append(p);return p
with patch.object(adapter.subprocess,'Popen',spawn):
 stream=adapter.render(t,0,180,R/'cancel');next(stream);stream.close()
assert len(children)==2 and all(p.poll() is not None for p in children)
# If audio cannot start, the already running video child must also be reaped.
children.clear()
def fail_second(*a,**kw):
 if children:raise OSError('injected audio spawn failure')
 return spawn(*a,**kw)
with patch.object(adapter.subprocess,'Popen',fail_second):
 try:list(adapter.render(t,58,66,R/'spawn-failure'))
 except OSError:pass
 else:raise AssertionError('spawn failure swallowed')
assert len(children)==1 and children[0].poll() is not None
report={'oracle':'fresh legacy full mixed-domain graph','comparisons':rows,'cancel_reaps_both':True,'spawn_failure_reaps_first':True}
(R/'summary.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
