"""Small, deterministic tagged media and an explicit six-second test timeline."""
from pathlib import Path
import subprocess,json,wave,math,struct
P=Path(__file__).resolve().parent;M=P/'media';M.mkdir(exist_ok=True)
def ff(name,args):
 with (P/'results'/f'fixture-{name}.log').open('w') as f:
  subprocess.run(['ffmpeg','-hide_banner','-nostdin','-y']+args,stdout=f,stderr=f,check=True)
for name,src in [('a','testsrc2=size=320x180:rate=30'),('b','testsrc=size=320x180:rate=30')]:
 ff(name,['-f','lavfi','-i',src,'-t','8','-an','-c:v','ffv1','-pix_fmt','yuv420p','-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709',str(M/(name+'.mkv'))])
for name in ['cue1','cue2']:
 ff(name,['-loop','1','-framerate','30','-i',str(M/(name+'.png')),'-t','6','-an','-c:v','ffv1','-pix_fmt','bgra',str(M/(name+'.mkv'))])
ff('overlay',['-f','lavfi','-i','color=c=black@0.0:size=320x180:rate=30,format=rgba,drawbox=x=35:y=25:w=100:h=65:color=red@0.55:t=fill:replace=1','-t','6','-an','-c:v','ffv1','-pix_fmt','bgra',str(M/'overlay.mkv')])
# A nonrepeating impulse/chirp train, then silence: makes missing reverb history observable.
with wave.open(str(M/'audio.wav'),'wb') as w:
 w.setnchannels(2);w.setsampwidth(2);w.setframerate(48000)
 data=bytearray()
 for i in range(8*48000):
  t=i/48000
  v=(.09*math.sin(2*math.pi*(190*t+47*t*t)) if t<3 else 0)
  if i in [100,11003,39209,60111,89993,137999]:v+=.6
  a=max(-32767,min(32767,round(v*32767)));data+=struct.pack('<hh',a,round(a*.73))
 w.writeframes(data)
with wave.open(str(M/'silence.wav'),'wb') as w:
 w.setnchannels(2);w.setsampwidth(2);w.setframerate(48000);w.writeframes(bytes(6*48000*4))
fixture={'version':1,'fps':[30,1],'width':320,'height':180,'frames':180,
 'clips':[
 {'id':'a','track':1,'start':0,'end':90,'src_in':0,'speed':[1,1],'asset':'a.mkv','kind':'video'},
 {'id':'b','track':2,'start':60,'end':150,'src_in':6,'speed':[3,2],'asset':'b.mkv','kind':'video'},
 {'id':'overlay','track':3,'start':20,'end':145,'src_in':0,'speed':[1,1],'asset':'overlay.mkv','kind':'video'},
 {'id':'cue1','track':4,'start':32,'end':76,'src_in':0,'speed':[1,1],'asset':'cue1.mkv','kind':'subtitle','text':'LOCAL RENDER • cue A'},
 {'id':'cue2','track':4,'start':85,'end':132,'src_in':0,'speed':[1,1],'asset':'cue2.mkv','kind':'subtitle','text':'Transition → speed 1.5×'},
 {'id':'audio','track':5,'start':0,'end':90,'src_in':0,'speed':[1,1],'asset':'audio.wav','kind':'audio'},
 {'id':'tail_padding','track':5,'start':90,'end':180,'src_in':0,'speed':[1,1],'asset':'silence.wav','kind':'audio'}],
 'crossfade':{'a':'a','b':'b','start':60,'end':90},
 'effects':[{'id':'temporal','type':'tmix','frames':5,'anchor':0,'end':180},
            {'id':'reverb','type':'sox.reverb','options':'reverb 70 50 100 100 10 0','anchor':0,'end':180}]}
(P/'fixture.json').write_text(json.dumps(fixture,ensure_ascii=False,indent=2))
