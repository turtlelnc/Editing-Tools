"""Diagnostic only: deliberately reuse a dirty MLT graph across backward seeks."""
import sys,json,subprocess,struct,hashlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter
P=adapter.P;R=P/'results';t=json.loads((P/'fixture.json').read_text());out=[]
for name,fx in [('combined',t['effects']),('temporal',[t['effects'][0]]),('stateless',[])]:
 timeline={**t,'effects':fx};ref=json.loads((R/f'ref-{name}'/'frames.json').read_text())
 for mode,replay in [('seek_only',58),('bounded',54),('replay_dirty_anchor',0)]:
  d=R/f'reused-{name}-{mode}';d.mkdir(exist_ok=True);adapter.profile(timeline,d/'profile');adapter.build_xml(timeline,adapter.plan(timeline,0,180),d/'timeline.xml')
  (d/'sequence.tsv').write_text(f'0 0 140\n{replay} 58 76\n')
  cmd=[str(P/'mlt_pull'),str(d/'timeline.xml'),str(d/'profile'),'0','0','180',str(d/'sequence.tsv')]
  frames=[]
  with (d/'stderr.log').open('w') as log:
   p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=log,env=adapter.environment())
   for _ in range(158):
    head=adapter.HEADER.unpack(adapter.read_exact(p.stdout,adapter.HEADER.size));video=adapter.read_exact(p.stdout,head[5]);audio=adapter.read_exact(p.stdout,head[4]*8)
    frames.append((head,hashlib.sha256(video).hexdigest(),hashlib.sha256(audio).hexdigest()))
   assert p.stdout.read(1)==b'';assert p.wait()==0
  v=[];a=[]
  for head,vhash,ahash in frames[140:]:
   if vhash!=ref[head[0]]['video_sha256']:v.append(head[0])
   if ahash!=ref[head[0]]['audio_sha256']:a.append(head[0])
  out.append({'case':name,'mode':mode,'video_mismatch_frames':v,'audio_mismatch_frames':a});print(out[-1],flush=True)
for r in out:
 assert bool(r['audio_mismatch_frames']) == (r['case']=='combined')
 assert bool(r['video_mismatch_frames']) == (r['case']!='stateless' and r['mode']=='seek_only')
(R/'session-probe.json').write_text(json.dumps(out,indent=2))
