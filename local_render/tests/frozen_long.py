"""Long immutable PCM bake: fresh full AV oracle, random reads and video merge.
Writes one ~661 MiB temporary PCM only if >=512 MiB disk reserve remains;
removes it in finally. Never removes any earlier run's files.
"""
import hashlib,json,os,random,shlex,shutil,subprocess,sys,tempfile,time,xml.etree.ElementTree as ET
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter
import frozen_audio as frozen
sys.path.insert(0,str(Path(__file__).resolve().parent));import long_replay as bench
P=adapter.P;ROOT=P.parent;R=P/'results/frozen-audio';R.mkdir(exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='long-',dir=R));bench.R=work
base=json.loads((P/'fixture.json').read_text());total=54180;t={**base,'frames':total}
expected_bytes=frozen.boundary(t,total)*8
if shutil.disk_usage(work).free<expected_bytes+frozen.RESERVE:
 raise OSError('Insufficient space for temporary long PCM plus 512 MiB reserve')
# Build the independent full-history oracle helper from the recorded source.
if os.environ.get('MLT_SYSTEM')=='1':
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','mlt-framework-7'],text=True))
else:
 flags=['-I',str(ROOT/'phase0/runtime/mlt/7.42.0/include/mlt-7'),str(ROOT/'phase0/runtime/mlt/7.42.0/lib/libmlt-7.7.42.0.dylib')]
subprocess.run(['cc','-O2','-Wall','-Wextra',str(P/'tests/long_replay_pull.c'),*flags,'-o',str(P/'long_replay_pull')],check=True)
# Fresh full mixed graph: identical semantics, independently evaluates BOTH domains.
bench.graph(1800,work/'full.xml')
for domain in ('video','audio'):
 subset,p=adapter.domain_plan(base,0,180,domain);effects=subset['effects'];subset={**subset,'effects':[]}
 xml=work/(domain+'.xml');(adapter.build_xml if domain=='video' else adapter.build_audio_xml)(subset,p,xml)
 tree=ET.parse(xml);root=tree.getroot();root.find("tractor[@id='main']").set('id','scene')
 playlist=ET.SubElement(root,'playlist',id='loop')
 for i in range((total+179)//180):ET.SubElement(playlist,'entry',producer='scene',**{'in':'0','out':str(min(180,total-i*180)-1)})
 main=ET.SubElement(root,'tractor',id='main',**{'in':'0','out':str(total-1)})
 ET.SubElement(main,'track',producer='loop')
 for fx in effects:
  node=ET.SubElement(main,'filter',**{'in':'0','out':str(total-1)})
  adapter.props(node,{'mlt_service':'avfilter.tmix','av.frames':fx['frames'],'av.threads':1,'position':'frame'} if domain=='video' else {'mlt_service':'sox','effect':fx['options']})
 ET.indent(root);tree.write(xml,encoding='utf-8',xml_declaration=True)
adapter.profile(t,work/'profile')
rng=random.Random(20261015)
ranges=[(0,8),(58,93),(129,136),(179,187),(1817,1847),(9054,9084),(54091,54121),(54179,54180)]
ranges += [(s:=rng.randrange(200,total-31),s+30) for _ in range(12)]
# Full oracle emits merged ordered windows, but still processes all 54,180 frames.
windows=[]
for a,b in sorted(ranges):
 if windows and a<=windows[-1][1]:windows[-1]=(windows[-1][0],max(b,windows[-1][1]))
 else:windows.append((a,b))
print('Generating fresh full AV oracle; no PCM file yet',flush=True)
ref,refstats=bench.execute('full-reference',work/'full.xml',0,total,-1,windows,240)
identity=hashlib.sha256(json.dumps({'fixed_scene_identity':frozen.timeline_identity(base),'total_frames':total,'semantics':'continuous root tmix and reverb over repeated scene'},sort_keys=True).encode()).hexdigest()
artifact=work/'snapshot';rows=[];removed=0;result=None
try:
 print('Baking complete effect-rendered audio to bounded temporary PCM',flush=True)
 meta=frozen._bake_graph(t,work/'audio.xml',work/'profile',artifact,identity)
 before=time.perf_counter();snapshot=frozen.FrozenAudio(artifact,identity);open_seconds=time.perf_counter()-before
 print('Baked',json.dumps(meta),'verified_open_seconds',open_seconds,flush=True)
 rng.shuffle(ranges)
 with snapshot:
  # Time repeated random reads after one whole-file integrity verification.
  for j,(start,end) in enumerate(ranges):
   before=time.perf_counter();blocks=list(snapshot.frames(start,end));read_seconds=time.perf_counter()-before
   assert len(blocks)==end-start
   for f in blocks:
    exp=ref[f['frame']]
    assert (hashlib.sha256(f['audio']).hexdigest(),f['sample_start'],f['samples'])==(exp['audio'],exp['sample_start'],exp['samples'])
   before=time.perf_counter();first=None;count=0
   video=adapter.render_graphs(t,start,end,work,[('video',work/'video.xml',max(0,start-4))])
   audio=snapshot.frames(start,end)
   try:
    for v,a in zip(video,audio,strict=True):
     if first is None:first=time.perf_counter()-before
     row={'video':hashlib.sha256(v['video']).hexdigest(),'audio':hashlib.sha256(a['audio']).hexdigest(),'position':v['mlt_position'],'sample_start':a['sample_start'],'samples':a['samples']}
     assert (v['frame'],v['sample_start'],v['samples'])==(a['frame'],a['sample_start'],a['samples'])
     assert row==ref[v['frame']],(v['frame'],row,ref[v['frame']]);count+=1
   finally:video.close();audio.close()
   row={'range':[start,end],'frames':count,'pcm_read_seconds':read_seconds,'first_merged_frame_seconds':first,'merged_complete_seconds':time.perf_counter()-before,'exact':True}
   rows.append(row);print('READ',json.dumps(row),flush=True)
 result={'seed':20261015,'scope':'synthetic 320x180 30fps repeated scene, continuous global effects; not real 4K; fixed long graphs built by test','full_reference':refstats,'artifact':meta,'verified_open_seconds':open_seconds,'reads_after_verified_open':True,'random_read_order':rows,'passed':True}
finally:
 path=artifact/'audio.f32le'
 if path.exists():removed=path.stat().st_size;path.unlink()
 if result is not None:
  result['temporary_pcm_removed_bytes']=removed
  (R/'long-summary.json').write_text(json.dumps(result,indent=2))
  (R/'long-hashes.json').write_text(json.dumps(ref,indent=2))
 print('Temporary PCM removed bytes',removed,flush=True)
