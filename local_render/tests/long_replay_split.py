"""Actual concurrent audio/video split and frame/sample recombination control."""
import sys,json,time,threading
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0,str(Path(__file__).resolve().parent));import long_replay as b
P=b.P;R=P/'results/long-replay/30min';b.R=R
ref={int(k):v for k,v in json.loads((R/'full-reference-1800/hashes.json').read_text()).items()}
control=json.loads((R/'controls.json').read_text());start=control['independent_audio']['range'][0][0];end=start+30;window=[(start,end)]
rows=[]
for rep in range(3):
 lock=threading.Lock();arrivals={};clock=time.perf_counter()
 def first(name):
  with lock:arrivals[name]=time.perf_counter()-clock
 def audio():return b.execute(f'split-audio-r{rep}',R/'audio-only.xml',0,end,4,window,90,lambda:first('audio'))
 def video():return b.execute(f'split-video-r{rep}',R/'graph-1800.xml',start-4,end,4,window,30,lambda:first('video'))
 with ThreadPoolExecutor(max_workers=2) as pool:
  fa=pool.submit(audio);fv=pool.submit(video);a,astats=fa.result();v,vstats=fv.result()
 merged={k:{'video':v[k]['video'],'audio':a[k]['audio'],'position':a[k]['position'],'sample_start':a[k]['sample_start'],'samples':a[k]['samples']} for k in a}
 exact=all(row==ref[k] and v[k]['position']==a[k]['position'] and v[k]['sample_start']==a[k]['sample_start'] and v[k]['samples']==a[k]['samples'] for k,row in merged.items())
 row={'repeat':rep,'range':window[0],'first_composite_frame_seconds':max(arrivals.values()),'first_video_seconds':arrivals['video'],'first_audio_seconds':arrivals['audio'],'range_complete_seconds':time.perf_counter()-clock,'exact':exact,'audio_peak_rss_bytes':astats['peak_rss_bytes'],'video_peak_rss_bytes':vstats['peak_rss_bytes']};rows.append(row)
 print('SPLIT',json.dumps(row),flush=True);assert exact
 (R/'split-parallel.json').write_text(json.dumps(rows,indent=2))
