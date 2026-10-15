"""Fixed-timeline bake, random PCM reads and merged previews versus full oracle."""
import copy,hashlib,json,random,shutil,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter
import frozen_audio as frozen
P=adapter.P;R=P/'results/frozen-audio';R.mkdir(exist_ok=True)
t=json.loads((P/'fixture.json').read_text());rng=random.Random(20261015);rows=[];timings=[];controls=[]
def sig(f):
 return {k:v for k,v in f.items() if k not in ('video','audio','source_position')}|{k:hashlib.sha256(f[k]).hexdigest() for k in ('video','audio')}
# A unique run directory preserves earlier evidence and prevents overwriting snapshots.
import tempfile
work=Path(tempfile.mkdtemp(prefix='short-',dir=R));removed=0
try:
 for name,c in [('combined',t),('ntsc',{**t,'fps':[30000,1001]}),('no-padding',{**t,'clips':[v for v in t['clips'] if v['id']!='tail_padding']})]:
  ref={f['frame']:sig(f) for f in adapter.render(c,0,180,work/name/'oracle',strategy='legacy')}
  artifact=work/name/'snapshot';meta=frozen.bake(c,artifact)
  before=time.perf_counter();snapshot=frozen.FrozenAudio(artifact,frozen.timeline_identity(c));opened=time.perf_counter()-before
  ranges=[(0,1),(58,93),(129,136),(159,180),(179,180)]
  ranges += [(s:=rng.randrange(0,175),min(180,s+rng.randrange(1,25))) for _ in range(10)]
  rng.shuffle(ranges)
  with snapshot:
   for j,(start,end) in enumerate(ranges):
    before=time.perf_counter();pcm=list(snapshot.frames(start,end));elapsed=time.perf_counter()-before
    assert len(pcm)==end-start
    for f in pcm:
     expected=ref[f['frame']]
     assert hashlib.sha256(f['audio']).hexdigest()==expected['audio']
     assert all(f[k]==expected[k] for k in ('sample_start','samples','global_audio_pts'))
    merged=[sig(f) for f in frozen.render(c,start,end,snapshot,work/name/f'preview-{j}')]
    assert merged==[ref[i] for i in range(start,end)]
    rows.append({'case':name,'range':[start,end],'exact_audio_and_merged_preview':True,'pcm_read_seconds':elapsed})
   changed=copy.deepcopy(c);changed['effects'][1]['options']='reverb 30 50 100 100 10 0'
   try:list(frozen.render(changed,58,66,snapshot,work/name/'stale'))
   except ValueError:controls.append({'case':name,'changed_timeline_rejected':True})
   else:raise AssertionError('Changed timeline accepted')
  timings.append({'case':name,'bytes':meta['bytes'],'bake_seconds':meta['bake_seconds'],'verified_open_seconds':opened,'sha256':meta['sha256']})
  # Detect same-length corruption; a mere file-size check is insufficient.
  with (artifact/'audio.f32le').open('r+b') as f:
   byte=f.read(1);f.seek(0);f.write(bytes([byte[0]^1]))
  try:frozen.FrozenAudio(artifact,meta['identity'])
  except ValueError:controls[-1]['same_length_corruption_rejected']=True
  else:raise AssertionError('Corruption accepted')
  path=artifact/'audio.f32le';removed+=path.stat().st_size;path.unlink()
 result={'seed':20261015,'comparisons':rows,'artifacts':timings,'controls':controls,'temporary_pcm_removed_bytes':removed,'passed':True}
 (R/'short-summary.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
finally:
 # Only remove reconstructible PCM files created by this unique test run.
 for path in work.glob('*/snapshot/audio.f32le'):path.unlink()
