"""Check a real lossless container in addition to the native frame/sample stream.
30fps/48k endpoints share a sample grid. NTSC is checked in the raw oracle test.
"""
import sys,json,subprocess,hashlib
from fractions import Fraction
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter
P=adapter.P;R=P/'results';t=json.loads((P/'fixture.json').read_text());records=[]
def ff(args):return subprocess.check_output(['ffmpeg','-v','error','-nostdin','-y']+args,stderr=subprocess.STDOUT)
full=R/'mux-full'
for name,begin,end in [('full',0,180),('transition',58,66),('subtitle-speed',129,136),('reverb-tail',159,174)]:
 d=full if name=='full' else R/f'mux-{name}';d.mkdir(exist_ok=True)
 if True:
  with (d/'video.rgba').open('wb') as v,(d/'audio.f32le').open('wb') as a:
   for f in adapter.render(t,begin,end,d):v.write(f['video']);a.write(f['audio'])
 dest=d/'render.nut'
 ff(['-f','rawvideo','-pixel_format','rgba','-video_size','320x180','-framerate','30','-i',str(d/'video.rgba'),'-f','f32le','-ar','48000','-ac','2','-i',str(d/'audio.f32le'),'-map','0:v:0','-map','1:a:0','-c:v','ffv1','-pix_fmt','bgra','-c:a','pcm_f32le',str(dest)])
 video=ff(['-i',str(dest),'-map','0:v:0','-pix_fmt','rgba','-fps_mode','passthrough','-f','rawvideo','-']);audio=ff(['-i',str(dest),'-map','0:a:0','-c:a','pcm_f32le','-f','f32le','-'])
 rawvideo=(d/'video.rgba').read_bytes();rawaudio=(d/'audio.f32le').read_bytes()
 with (full/'video.rgba').open('rb') as f:f.seek(begin*320*180*4);expectedv=f.read((end-begin)*320*180*4)
 with (full/'audio.f32le').open('rb') as f:f.seek(begin*1600*8);expecteda=f.read((end-begin)*1600*8)
 info=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_packets','-of','json',str(dest)]));streams=info['streams'];tb={s['index']:Fraction(s['time_base']) for s in streams};vp=[p for p in info['packets'] if p['codec_type']=='video'];ap=[p for p in info['packets'] if p['codec_type']=='audio']
 correctpts=len(vp)==end-begin and all(int(p['pts'])*tb[p['stream_index']]==Fraction(i,30) for i,p in enumerate(vp))
 audio_pts=ap[0]['pts']==0 and all(int(p['pts'])*tb[p['stream_index']]==Fraction(sum(int(q['duration']) for q in ap[:i]),48000) for i,p in enumerate(ap))
 audio_duration=sum(int(p['duration'])*tb[p['stream_index']] for p in ap)==Fraction((end-begin)*1600,48000)
 x={'range':[begin,end],'container':'NUT/FFV1/PCM_f32le','global_video_origin':[begin,30],'global_audio_origin_samples':begin*1600,'video_bit_exact':video==rawvideo==expectedv,'audio_bit_exact':audio==rawaudio==expecteda,'video_pts_exact':correctpts,'audio_packet_pts_exact':audio_pts,'audio_duration_exact':audio_duration,'stream_time_bases':{str(i):str(v) for i,v in tb.items()},'video_packets':len(vp),'audio_samples_per_channel':len(audio)//8}
 (d/'ffprobe.json').write_text(json.dumps(info,indent=2));records.append(x);print(x,flush=True)
 assert all(x[k] for k in ['video_bit_exact','audio_bit_exact','video_pts_exact','audio_packet_pts_exact','audio_duration_exact'])
(R/'mux-verification.json').write_text(json.dumps(records,indent=2))
