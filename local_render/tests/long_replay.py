"""Streaming long-history screening; no raw frames or long media files on disk."""
from pathlib import Path
import sys,copy,json,subprocess,time,hashlib,threading,os,signal,shutil,xml.etree.ElementTree as ET
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter
P=adapter.P;R=P/'results/long-replay';R.mkdir(parents=True,exist_ok=True)
base=json.loads((P/'fixture.json').read_text())

def graph(seconds,path):
 t=copy.deepcopy(base);t['effects']=[]
 adapter.build_xml(t,adapter.plan(t,0,180),path)
 tree=ET.parse(path);root=tree.getroot();root.find("tractor[@id='main']").set('id','scene')
 n=seconds*30+180;root.set('producer','long-main')
 playlist=ET.SubElement(root,'playlist',id='loop')
 for i in range((n+179)//180):ET.SubElement(playlist,'entry',producer='scene',**{'in':'0','out':str(min(180,n-i*180)-1)})
 main=ET.SubElement(root,'tractor',id='long-main',**{'in':'0','out':str(n-1)})
 ET.SubElement(main,'track',producer='loop')
 for fx in base['effects']:
  node=ET.SubElement(main,'filter',**{'in':'0','out':str(n-1)})
  if fx['type']=='tmix':adapter.props(node,{'mlt_service':'avfilter.tmix','av.frames':5,'av.threads':1,'position':'frame'})
  else:adapter.props(node,{'mlt_service':'sox','effect':fx['options']})
 ET.indent(root);tree.write(path,encoding='utf-8',xml_declaration=True)
 adapter.profile(base,path.with_suffix('.profile'));return n

def execute(name,xml,replay,end,history,windows,timeout=240,on_first=None):
 if shutil.disk_usage(R).free<512*1024*1024:raise RuntimeError('Need 512 MiB free; no large outputs will be created')
 d=R/name;d.mkdir(exist_ok=True);(d/'windows.txt').write_text(''.join(f'{a} {b}\n' for a,b in windows))
 cmd=[str(P/'long_replay_pull'),str(xml),str(xml.with_suffix('.profile')),str(replay),str(end),str(history),str(d/'windows.txt'),str(d/'native.json')]
 (d/'command.json').write_text(json.dumps(cmd,indent=2))
 output={};observed_first=None;started=time.perf_counter();expired=[]
 with (d/'stderr.log').open('w') as log:
  proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=log,env=adapter.environment(),start_new_session=True)
  def kill():
   expired.append(True)
   try:os.killpg(proc.pid,signal.SIGKILL)
   except ProcessLookupError:pass
  timer=threading.Timer(timeout,kill);timer.start()
  try:
   for a,b in windows:
    for i in range(a,b):
     h=adapter.HEADER.unpack(adapter.read_exact(proc.stdout,adapter.HEADER.size))
     v=adapter.read_exact(proc.stdout,h[5]);audio=adapter.read_exact(proc.stdout,h[4]*8)
     if observed_first is None:observed_first=time.perf_counter()-started
     output[h[0]]={'video':hashlib.sha256(v).hexdigest(),'audio':hashlib.sha256(audio).hexdigest(),'position':h[1],'sample_start':h[3],'samples':h[4]}
     if len(output)==1 and on_first is not None:on_first()
   assert not proc.stdout.read(1)
   if proc.wait(timeout=10):raise RuntimeError(f'Nonzero helper exit {proc.returncode}')
  except Exception as e:
   (d/'failure.json').write_text(json.dumps({'error':str(e),'timed_out':bool(expired),'elapsed':time.perf_counter()-started},indent=2));raise
  finally:
   timer.cancel();proc.stdout.close()
   if proc.poll() is None:kill();proc.wait()
 stats=json.loads((d/'native.json').read_text());stats.update({'observed_first_frame_seconds':observed_first,'observed_total_seconds':time.perf_counter()-started,'name':name,'history_video_frames':history,'range':windows,'replay_begin':replay})
 assert len(output)==sum(b-a for a,b in windows)
 assert all(row['position']==f and row['sample_start']==f*1600 and row['samples']==1600 for f,row in output.items())
 (d/'hashes.json').write_text(json.dumps(output,indent=2));(d/'stats.json').write_text(json.dumps(stats,indent=2))
 print(json.dumps(stats),flush=True);return output,stats

if __name__=='__main__':
 import argparse
 ap=argparse.ArgumentParser();ap.add_argument('--targets',default='60');ap.add_argument('--timeout',type=int,default=240);ap.add_argument('--repeats',type=int,default=1);ap.add_argument('--label',default='run');args=ap.parse_args()
 R=R/args.label;R.mkdir(parents=True,exist_ok=True)
 targets=[int(v) for v in args.targets.split(',')];xml=R/f'graph-{max(targets)}.xml';total=graph(max(targets),xml)
 windows=[(s*30+(j*37+17)%150,s*30+(j*37+17)%150+30) for j,s in enumerate(targets)]
 ref,refstats=execute(f'full-reference-{max(targets)}',xml,0,total,-1,windows,args.timeout)
 rows=[]
 for rep in range(args.repeats):
  for index,s in enumerate(targets):
   w=[windows[index]]
   for mode,hist in [('full_av',-1),('audio_history_video4',4)]:
    out,stats=execute(f'{mode}-{s}-r{rep}',xml,0,w[0][1],hist,w,args.timeout)
    stats['exact']=all(v==ref[k] for k,v in out.items());stats['target_seconds']=s;rows.append(stats)
    summary={'profile':{'fps':[30,1],'resolution':[320,180],'audio_rate':48000,'channels':2},'workload':'synthetic repeating six-second mixed scene; feedback reverb and tmix at global root','reference':refstats,'trials':rows,'exact_failures':[r['name'] for r in rows if not r['exact']]}
    (R/'summary.json').write_text(json.dumps(summary,indent=2))
    assert stats['exact'],stats['name']
