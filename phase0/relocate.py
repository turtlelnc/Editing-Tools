import pathlib,subprocess
root=pathlib.Path(__file__).resolve().parent/'runtime'
for p in root.rglob('*'):
 if not p.is_file() or p.is_symlink():continue
 if p.read_bytes()[:4] not in [bytes.fromhex('cffaedfe'),bytes.fromhex('cafebabe')]:continue
 s=subprocess.check_output(['otool','-L',str(p)],text=True)
 for line in s.splitlines()[1:]:
  old=line.strip().split(' (')[0]
  if '@@HOMEBREW_PREFIX@@' in old or '@@HOMEBREW_CELLAR@@' in old or old.startswith('@rpath/'):
   new=old.replace('@@HOMEBREW_PREFIX@@','/opt/homebrew').replace('@@HOMEBREW_CELLAR@@',str(root))
   if old.startswith('@rpath/'):
    candidates=[q for q in root.rglob(old[7:]) if q.is_file()]
    if candidates:new=str(candidates[0])
   for name,version in [('mlt','7.42.0'),('gstreamer','1.28.8')]:
    new=new.replace(f'/opt/homebrew/opt/{name}/',str(root/name/version)+'/')
   subprocess.run(['install_name_tool','-change',old,new,str(p)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 if p.suffix=='.dylib':
  subprocess.run(['install_name_tool','-id',str(p),str(p)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 subprocess.run(['codesign','--force','--sign','-',str(p)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
