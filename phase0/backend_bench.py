import os,pathlib,subprocess,time,json,signal,shutil
P=pathlib.Path(__file__).resolve().parent;R=P/'results';M=P/'media';rt=P/'runtime'; rows=[]
env=os.environ.copy();env.update({'DYLD_LIBRARY_PATH':str(rt/'mlt/7.42.0/lib')+':'+str(rt/'gstreamer/1.28.8/lib'),'MLT_REPOSITORY':str(rt/'mlt/7.42.0/lib/mlt'),'MLT_DATA':str(rt/'mlt/7.42.0/share/mlt')})
# Only load the modules needed by this workload, avoids optional Qt/GPL modules.
mods=rt/'mlt-minimal';mods.mkdir(exist_ok=True)
for name in ['core','avformat','xml']:
 target=mods/f'libmlt{name}.so'
 if not target.exists():target.symlink_to(rt/f'mlt/7.42.0/lib/mlt/libmlt{name}.so')
env['MLT_REPOSITORY']=str(mods)
def run(name,args,timeout=120):
 t=time.perf_counter();log=R/(name+'.log')
 with log.open('w') as f:
  p=subprocess.Popen(['/usr/bin/time','-l']+args,env=env,stdout=f,stderr=f,start_new_session=True)
  try:code=p.wait(timeout=timeout)
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait();code=-9
 rss=next((int(l.split()[0]) for l in log.read_text().splitlines() if 'maximum resident set size' in l),None)
 x={'name':name,'command':args,'exit':code,'wall_seconds':time.perf_counter()-t,'peak_rss_bytes':rss};rows.append(x);(R/'backend.json').write_text(json.dumps(rows,indent=2));print(json.dumps(x),flush=True)
 return code
melt=str(rt/'mlt/7.42.0/bin/melt')
profile=R/'profile';profile.write_text('description=Phase0\nframe_rate_num=30\nframe_rate_den=1\nwidth=640\nheight=360\nprogressive=1\nsample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n')
for n in [10,1000,10000,100000]:
 # One frame per timeline entry. Same asset shared, explicitly not N unique media files.
 xml=R/f'mlt-{n}.xml';xml.write_text('<mlt><producer id="p"><property name="resource">'+str(M/'h264.mov')+'</property></producer><playlist id="main">'+''.join('<entry producer="p" in="0" out="0"/>' for _ in range(n))+'</playlist><tractor id="top" in="0" out="0"><track producer="main"/></tractor></mlt>')
 run(f'mlt-load-{n}',[str(P/'mlt_probe'),str(xml),str(profile)])
 if n==1000:
  full=R/'mlt-full-1000.xml';full.write_text(xml.read_text().replace('id="top" in="0" out="0"','id="top" in="0" out="999"'))
  run('mlt-render-1000',[melt,str(full),'-profile',str(profile),'-consumer',f'avformat:{M}/mlt-1000.mkv','vcodec=ffv1','an=1','real_time=-1'])
run('mlt-render-single',[melt,str(M/'h264.mov'),'-profile',str(profile),'in=0','out=119','-consumer',f'avformat:{M}/mlt-single.mkv','vcodec=ffv1','an=1','real_time=-1'])
# 8 layers, top layer covers others. Decode/compositor smoke; no claim of meaningful blend correctness.
xml=R/'mlt-8.xml';xml.write_text('<mlt>'+''.join(f'<producer id="p{i}"><property name="resource">{M}/h264.mov</property></producer><playlist id="t{i}"><entry producer="p{i}" in="0" out="119"/></playlist>' for i in range(8))+'<tractor id="main" in="0" out="119"><multitrack>'+''.join(f'<track producer="t{i}"/>' for i in range(8))+'</multitrack>'+''.join(f'<transition in="0" out="119"><property name="mlt_service">composite</property><property name="a_track">0</property><property name="b_track">{i}</property></transition>' for i in range(1,8))+'</tractor></mlt>')
run('mlt-render-8',[melt,str(xml),'-profile',str(profile),'-consumer',f'avformat:{M}/mlt-8.mkv','vcodec=ffv1','an=1','real_time=-1'])
