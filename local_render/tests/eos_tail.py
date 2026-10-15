"""Silence samples and an empty MLT track are distinct for stateful audio tails."""
import sys,json,math,struct,copy
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter
P=adapter.P;t=json.loads((P/'fixture.json').read_text());assert any(c['id']=='tail_padding' for c in t['clips']);rows=[]
for padding in [True,False]:
 c=copy.deepcopy(t)
 if not padding:c['clips']=[v for v in c['clips'] if v['id']!='tail_padding']
 energy=0;count=0
 for f in adapter.render(c,120,180,P/'results'/f'eos-pad-{padding}'):
  a=struct.unpack('<'+'f'*(len(f['audio'])//4),f['audio']);energy+=sum(v*v for v in a);count+=len(a)
 rows.append({'explicit_pcm_padding':padding,'tail_rms':math.sqrt(energy/count)})
print(rows);(P/'results/eos-tail.json').write_text(json.dumps(rows,indent=2));assert rows[0]['tail_rms']>1e-5 and rows[1]['tail_rms']==0
