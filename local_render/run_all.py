"""Parallel independent verifications; state-reuse diagnostic follows its oracle."""
from pathlib import Path
import os,subprocess,time,json,sys,shutil
from concurrent.futures import ThreadPoolExecutor
P=Path(__file__).resolve().parent;ROOT=P.parent;R=P/'results';R.mkdir(exist_ok=True)
if shutil.disk_usage(P).free < 512*1024*1024:
 print('Need 512 MiB free for bounded verification intermediates; no files deleted.');sys.exit(2)
LIB=ROOT/'phase0/runtime/mlt/7.42.0/lib/libmlt-7.7.42.0.dylib';INC=ROOT/'phase0/runtime/mlt/7.42.0/include/mlt-7'
def command(name,args):
 start=time.perf_counter()
 with (R/(name+'.run.log')).open('w') as log:
  code=subprocess.call(args,cwd=ROOT,stdout=log,stderr=log)
 return {'task':name,'exit':code,'seconds':time.perf_counter()-start}
compiles=[('build-pull',['clang','-O2','-Wall','-Wextra','-I',str(INC),str(P/'src/mlt_pull.c'),str(LIB),'-o',str(P/'mlt_pull')]),('build-subtitle',['swiftc',str(P/'src/subtitle_assets.swift'),'-o',str(P/'subtitle_assets')]),('build-clock',['clang','-O2','-I',str(INC),str(P/'tests/sample_clock.c'),str(LIB),'-o',str(P/'sample_clock')])]
SYSTEM=os.environ.get('MLT_SYSTEM')=='1'
if SYSTEM:
 import shlex
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','mlt-framework-7'],text=True))
 compiles=[('build-pull',['cc','-O2','-Wall','-Wextra',str(P/'src/mlt_pull.c'),*flags,'-o',str(P/'mlt_pull')]),('build-clock',['cc','-O2',str(P/'tests/sample_clock.c'),*flags,'-o',str(P/'sample_clock')])]
with ThreadPoolExecutor(max_workers=3) as pool:builds=list(pool.map(lambda task:command(*task),compiles))
if any(r['exit'] for r in builds):print(builds);sys.exit(1)
if not SYSTEM:
 mods=P/'runtime-modules';mods.mkdir(exist_ok=True)
 for n in ['core','xml','avformat','sox']:
  target=mods/f'libmlt{n}.so'
  if not target.exists():target.symlink_to(LIB.parent/'mlt'/target.name)
setup=[('fixture',[sys.executable,str(P/'make_fixture.py')])]
if not SYSTEM:setup.insert(0,('subtitle-assets',[str(P/'subtitle_assets'),str(P/'media')]))
else:
 for name in ['cue1.png','cue2.png']:
  if not (P/'media'/name).exists():raise RuntimeError('Transfer the two subtitle PNG fixtures before cloud execution')
for name,args in setup:
 r=command(name,args)
 if r['exit']:print(r);sys.exit(1)
# Each worker has distinct output directories; no shared mutable renderer state.
tasks=[('verify',[sys.executable,str(P/'tests/verify.py')]),('mux',[sys.executable,str(P/'tests/mux_roundtrip.py')]),('contracts',[sys.executable,str(P/'tests/contracts.py')]),('eos',[sys.executable,str(P/'tests/eos_tail.py')]),('clock',[str(P/'sample_clock')])]
with ThreadPoolExecutor(max_workers=4) as pool:results=list(pool.map(lambda task:command(*task),tasks))
# Clock stdout is a JSON evidence file, not a pass/fail for the unsupported interval.
(R/'sample-clock.json').write_text((R/'clock.run.log').read_text())
if not next(r['exit'] for r in results if r['task']=='verify'):
 results.append(command('session',[sys.executable,str(P/'tests/session_probe.py')]))
from cleanup_generated import cleanup
cleaned=cleanup()
summary={'cleanup':cleaned,'builds':builds,'tests':results,'passed':all(r['exit']==0 for r in results)}
(R/'SUITE.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2));sys.exit(0 if summary['passed'] else 1)
