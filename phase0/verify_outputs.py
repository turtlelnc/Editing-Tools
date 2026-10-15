import subprocess,json,pathlib,hashlib
P=pathlib.Path(__file__).resolve().parent;R=P/'results';M=P/'media'
def hashes(path):
 p=subprocess.run(['ffmpeg','-v','error','-i',str(path),'-map','0:v:0','-vf','extractplanes=y','-f','framemd5','-'],capture_output=True,text=True)
 return [l.split(',')[-1].strip() for l in p.stdout.splitlines() if l and not l.startswith('#')],p.returncode
ref,_=hashes(M/'h264.mov');out={}
for name in ['mlt-single','ges-single','mlt-8','ges-8','mlt-1000','ges-100','ges-1000','ges-1000-isolated']:
 path=M/(name+'.mkv')
 if not path.exists():continue
 h,rc=hashes(path);expect=ref if 'single' in name or name.endswith('-8') else [ref[0]]*(1000 if name.endswith('-isolated') else int(name.split('-')[-1]));out[name]={'decode_exit':rc,'frames':len(h),'expected_frames':len(expect),'luma_different_frames':sum(a!=b for a,b in zip(h,expect)),'luma_exact':h==expect,'file_bytes':path.stat().st_size}
(R/'output-verification.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
