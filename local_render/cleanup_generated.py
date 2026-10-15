"""Remove only named, reconstructible test raw buffers; retain lossless evidence."""
from pathlib import Path
import json
P=Path(__file__).resolve().parent;R=P/'results'
def cleanup():
 rows=[]
 for name in ['smoke','mux-full','mux-transition','mux-subtitle-speed','mux-reverb-tail']:
  for filename in ['video.rgba','audio.f32le']:
   path=R/name/filename
   if path.exists():rows.append({'file':str(path),'bytes':path.stat().st_size});path.unlink()
 result={'removed':rows,'bytes_reclaimed':sum(v['bytes'] for v in rows)}
 (R/'cleanup-latest.json').write_text(json.dumps(result,indent=2));return result
if __name__=='__main__':print(cleanup())
