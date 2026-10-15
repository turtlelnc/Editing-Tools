import os,pathlib,subprocess,time,json,signal
P=pathlib.Path(__file__).resolve().parent;R=P/'results';M=P/'media';rt=P/'runtime/gstreamer/1.28.8';env=os.environ.copy();env.update({'GST_PLUGIN_SCANNER':str(rt/'libexec/gstreamer-1.0/gst-plugin-scanner'),'GST_PLUGIN_SYSTEM_PATH':str(rt/'lib/gstreamer-1.0'),'GST_REGISTRY':str(R/'gst-registry.bin')});rows=[]
def run(name,args,timeout=120):
 t=time.perf_counter();log=R/(name+'.log')
 with log.open('w') as f:
  p=subprocess.Popen(['/usr/bin/time','-l']+args,env=env,stdout=f,stderr=f,start_new_session=True)
  try:code=p.wait(timeout=timeout)
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait();code=-9
 rss=next((int(l.split()[0]) for l in log.read_text().splitlines() if 'maximum resident set size' in l),None);x={'name':name,'command':args,'exit':code,'wall_seconds':time.perf_counter()-t,'peak_rss_bytes':rss};rows.append(x);(R/'ges.json').write_text(json.dumps(rows,indent=2));print(json.dumps(x),flush=True);return code
base=[str(rt/'bin/ges-launch-1.0'),'--no-interactive','-t','video','--video-caps','video/x-raw,width=640,height=360,framerate=30/1']
run('ges-save-one',base+['--save-only',str(R/'ges-one.xges'),'+clip',str(M/'h264.mov'),'duration=4000000000'])
run('ges-render-single',base+['-o',str(M/'ges-single.mkv'),'-f','video/x-matroska:video/x-ffv,ffvversion=1','+clip',str(M/'h264.mov'),'duration=4000000000'])
# 1000 one-frame clips generated in native XGES from the saved project template.
import xml.etree.ElementTree as ET
if (R/'ges-one.xges').exists():
 for n in [100,1000]:
  tree=ET.parse(R/'ges-one.xges');layer=tree.find('.//layer');clip=layer.find('clip');layer.remove(clip)
  import copy
  for i in range(n):
   x=copy.deepcopy(clip);x.set('id',str(i));x.set('start',str(round(i*1e9/30)));x.set('duration',str(round((i+1)*1e9/30)-round(i*1e9/30)));x.set('inpoint','0');x.set('properties',f'properties, name=(string)clip{i};');layer.append(x)
  path=R/f'ges-{n}.xges';tree.write(path,encoding='utf-8',xml_declaration=True)
  run(f'ges-render-{n}',base+['-l',str(path),'-o',str(M/f'ges-{n}.mkv'),'-f','video/x-matroska:video/x-ffv,ffvversion=1'])
 args=[]
 for i in range(8):args+=['+clip',str(M/'h264.mov'),'start=0','duration=4000000000',f'layer={i}']
 run('ges-render-8',base+['-o',str(M/'ges-8.mkv'),'-f','video/x-matroska:video/x-ffv,ffvversion=1']+args)
