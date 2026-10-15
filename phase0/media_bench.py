import subprocess,time,json,pathlib,os,signal
P=pathlib.Path(__file__).resolve().parent; R=P/'results'; M=P/'media'; results=[]
def run(name,args,timeout=90):
 log=R/(name+'.log'); cmd=['/usr/bin/time','-l']+args; start=time.perf_counter()
 with log.open('w') as f:
  p=subprocess.Popen(cmd,stdout=f,stderr=f,start_new_session=True)
  try: code=p.wait(timeout=timeout)
  except subprocess.TimeoutExpired: os.killpg(p.pid,signal.SIGKILL);p.wait();code=-9
 s=log.read_text(); rss=None
 for l in s.splitlines():
  if 'maximum resident set size' in l: rss=int(l.split()[0])
 x={'name':name,'command':args,'exit':code,'wall_seconds':time.perf_counter()-start,'peak_rss_bytes':rss,'log':str(log)}; results.append(x); (R/'media.json').write_text(json.dumps(results,indent=2));print(json.dumps(x),flush=True);return code
ff=['ffmpeg','-hide_banner','-nostdin','-y']
run('make-h264',ff+['-f','lavfi','-i','testsrc2=size=640x360:rate=30','-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t','4','-c:v','libx264','-g','120','-pix_fmt','yuv420p','-c:a','pcm_s16le',str(M/'h264.mov')])
run('make-proxy',ff+['-i',str(M/'h264.mov'),'-c:v','prores_ks','-profile:v','0','-c:a','copy',str(M/'proxy.mov')])
for source in ['h264','proxy']:
 for mode in ['software','videotoolbox']:
  hw=['-hwaccel','videotoolbox'] if mode=='videotoolbox' else []
  run(f'decode-{source}-{mode}',ff+hw+['-i',str(M/(source+'.mov')),'-map','0:v:0','-an','-f','null','-'])
 for j,t in enumerate([.1,1.3,2.7]): run(f'seek-{source}-{j}',ff+['-ss',str(t),'-i',str(M/(source+'.mov')),'-frames:v','1','-an','-f','framemd5','-'])
for n in [100,500,1000]:
 # Independent demuxers, one thread each, first output frame from all inputs.
 args=ff+['-loglevel','warning','-filter_complex_threads','1']
 for i in range(n): args+=['-threads','1','-i',str(M/'h264.mov')]
 graph=''.join(f'[{i}:v]trim=end_frame=1,setpts=PTS-STARTPTS[v{i}];' for i in range(n))+''.join(f'[v{i}]' for i in range(n))+f'concat=n={n}:v=1:a=0[out]'
 script=R/f'inputs-{n}.filter';script.write_text(graph)
 run(f'inputs-{n}',args+['-/filter_complex',str(script),'-map','[out]','-an','-frames:v',str(n),'-f','null','-'],timeout=120)
# Full vs segmented, lossless reference, stateful temporal filter deliberate counterexample.
for state in ['null','tmix=frames=3']:
 label='stateless' if state=='null' else 'temporal'
 run(f'full-{label}',ff+['-i',str(M/'h264.mov'),'-vf',state,'-an','-c:v','ffv1',str(M/f'full-{label}.mkv')])
 for j in [0,1]: run(f'part-{label}-{j}',ff+['-ss',str(j*2),'-i',str(M/'h264.mov'),'-t','2','-vf',state,'-an','-c:v','ffv1',str(M/f'part-{label}-{j}.mkv')])
 cat=R/f'{label}.concat';cat.write_text(''.join(f"file '{M}/part-{label}-{j}.mkv'\n" for j in [0,1]))
 run(f'join-{label}',ff+['-f','concat','-safe','0','-i',str(cat),'-c','copy',str(M/f'join-{label}.mkv')])
 for variant in ['full','join']: run(f'hash-{variant}-{label}',ff+['-i',str(M/f'{variant}-{label}.mkv'),'-map','0:v','-f','framemd5',str(R/f'{variant}-{label}.md5')])
 a=[l.split(',')[-1].strip() for l in (R/f'full-{label}.md5').read_text().splitlines() if not l.startswith('#')];b=[l.split(',')[-1].strip() for l in (R/f'join-{label}.md5').read_text().splitlines() if not l.startswith('#')]
 (R/f'compare-{label}.json').write_text(json.dumps({'frames_full':len(a),'frames_join':len(b),'different_frames':[i for i,(x,y) in enumerate(zip(a,b)) if x!=y]},indent=2))
