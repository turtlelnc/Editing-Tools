"""Production domain helpers/scheduler on the existing 30-minute oracle workload.
The benchmark constructs loop XML; the public fixed-layout JSON is not generalized.
"""
import hashlib,json,sys,time,xml.etree.ElementTree as ET
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter
P=adapter.P;R=P/'results/split-long';R.mkdir(exist_ok=True)
t=json.loads((P/'fixture.json').read_text());start,end=54091,54121;total=54180
ref=json.loads((P.parent/'evidence/macos/long-replay/full-reference-1800/hashes.json').read_text())
for domain in ('video','audio'):
 subset,p=adapter.domain_plan(t,0,180,domain);effects=subset['effects'];subset={**subset,'effects':[]}
 xml=R/(domain+'.xml');(adapter.build_xml if domain=='video' else adapter.build_audio_xml)(subset,p,xml)
 tree=ET.parse(xml);root=tree.getroot();root.find("tractor[@id='main']").set('id','scene');root.set('producer','main')
 pl=ET.SubElement(root,'playlist',id='loop')
 for i in range((total+179)//180):ET.SubElement(pl,'entry',producer='scene',**{'in':'0','out':str(min(180,total-i*180)-1)})
 main=ET.SubElement(root,'tractor',id='main',**{'in':'0','out':str(total-1)})
 ET.SubElement(main,'track',producer='loop')
 for fx in effects:
  node=ET.SubElement(main,'filter',**{'in':'0','out':str(total-1)})
  adapter.props(node,{'mlt_service':'avfilter.tmix','av.frames':fx['frames'],'av.threads':1,'position':'frame'} if domain=='video' else {'mlt_service':'sox','effect':fx['options']})
 ET.indent(root);tree.write(xml,encoding='utf-8',xml_declaration=True)
adapter.profile(t,R/'profile');rows=[]
for rep in range(3):
 before=time.perf_counter();first=None;hashes={}
 for f in adapter.render_graphs(t,start,end,R,[('video',R/'video.xml',start-4),('audio',R/'audio.xml',0)]):
  if first is None:first=time.perf_counter()-before
  row={'video':hashlib.sha256(f['video']).hexdigest(),'audio':hashlib.sha256(f['audio']).hexdigest(),'position':f['mlt_position'],'sample_start':f['sample_start'],'samples':f['samples']}
  assert row==ref[str(f['frame'])],f['frame'];hashes[str(f['frame'])]=row
 row={'repeat':rep,'range':[start,end],'first_frame_seconds':first,'complete_seconds':time.perf_counter()-before,'frames':len(hashes),'exact':True}
 rows.append(row);print(json.dumps(row),flush=True)
 (R/f'hashes-{rep}.json').write_text(json.dumps(hashes,indent=2))
(R/'summary.json').write_text(json.dumps({'reference':'evidence/macos/long-replay/full-reference-1800/hashes.json','path':'production mlt_pull domain modes + render_graphs; benchmark loop XML','trials':rows},indent=2))
