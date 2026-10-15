"""Failed PCM builds cannot become usable snapshots or overwrite older results."""
import json,sys,tempfile
from pathlib import Path
from collections import namedtuple
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter,frozen_audio as frozen
P=adapter.P;R=P/'results/frozen-audio';R.mkdir(exist_ok=True);work=Path(tempfile.mkdtemp(prefix='contracts-',dir=R))
t=json.loads((P/'fixture.json').read_text());adapter.profile(t,work/'profile');rows=[]
Usage=namedtuple('Usage','total used free')
with patch.object(frozen.shutil,'disk_usage',return_value=Usage(1,1,0)):
 try:frozen._bake_graph(t,work/'irrelevant.xml',work/'profile',work/'no-space','identity')
 except OSError:pass
 else:raise AssertionError('Disk guard bypassed')
assert not (work/'no-space').exists();rows.append('insufficient_disk_rejected_before_PCM_creation')
def interrupted(*args,**kwargs):
 yield {'frame':0,'audio':b'\x00'*(1600*8)}
 raise RuntimeError('injected interrupted DSP stream')
with patch.object(adapter,'render_graphs',interrupted):
 try:frozen._bake_graph(t,work/'irrelevant.xml',work/'profile',work/'interrupted','identity')
 except RuntimeError:pass
 else:raise AssertionError('Interrupted bake accepted')
assert not list((work/'interrupted').glob('audio.*')) and not (work/'interrupted/manifest.json').exists()
rows.append('interrupted_PCM_discarded_no_manifest')
old=work/'existing';old.mkdir();(old/'keep.txt').write_text('preserve')
try:frozen._bake_graph(t,work/'irrelevant.xml',work/'profile',old,'identity')
except FileExistsError:pass
else:raise AssertionError('Existing artifact overwritten')
assert (old/'keep.txt').read_text()=='preserve';rows.append('existing_artifact_preserved')
# After a completed bake, source validation failure must invalidate its manifest.
artifact=work/'source-race';graphdir=work/'source-race-graph';graphdir.mkdir();artifact.mkdir()
(artifact/'manifest.json').write_text('{}');(artifact/'audio.f32le').write_bytes(b'bad')
with patch.object(frozen,'timeline_identity',side_effect=['first',OSError('source disappeared')]),patch.object(frozen,'_bake_graph',return_value={}),patch.object(Path,'mkdir'):
 try:frozen.bake(t,artifact)
 except OSError:pass
 else:raise AssertionError('Source mutation accepted')
assert not (artifact/'manifest.json').exists() and not (artifact/'audio.f32le').exists()
rows.append('post_bake_source_validation_failure_discards_artifact')
(R/'contracts.json').write_text(json.dumps({'passed':True,'controls':rows},indent=2));print(rows)
