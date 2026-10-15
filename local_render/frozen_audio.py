"""Explicit immutable effect-rendered PCM for one fixed timeline.

No lookup, eviction, incremental invalidation, or DSP state serialization. The
caller owns the artifact and must explicitly bake a new one after edits.
"""
from pathlib import Path
from fractions import Fraction
import hashlib,json,os,shutil,time
import adapter

RATE=48000
CHANNELS=2
SAMPLE_BYTES=4
RESERVE=512*1024*1024

def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()

def boundary(t,frame):
    n,d=t['fps'];return (frame*RATE*d*2+n)//(2*n)

def timeline_identity(t):
    adapter.plan(t,0,t['frames'])
    # Conservative fixed-snapshot identity: even video-only edits require rebake.
    files={c['asset']:digest(adapter.P/'media'/c['asset']) for c in t['clips']}
    payload={'timeline':t,'source_sha256':files}
    return hashlib.sha256(json.dumps(payload,sort_keys=True,separators=(',',':')).encode()).hexdigest()

def _bake_graph(t,xml,profile_path,directory,identity):
    """Internal streaming writer; callers must validate graph semantics and identity."""
    directory=Path(directory)
    expected=boundary(t,t['frames'])*CHANNELS*SAMPLE_BYTES
    directory.parent.mkdir(parents=True,exist_ok=True)
    if shutil.disk_usage(directory.parent).free < expected+RESERVE:
        raise OSError(f'Need {expected} bytes plus {RESERVE} bytes free reserve; no files deleted')
    directory.mkdir()  # Refuse overwrite, including incomplete artifacts.
    work=directory/'build';work.mkdir()
    shutil.copyfile(profile_path,work/'profile')
    staged=directory/'audio.f32le.partial';pcm=directory/'audio.f32le'
    h=hashlib.sha256();started=time.perf_counter();written=0
    stream=adapter.render_graphs(t,0,t['frames'],work,[('audio',Path(xml),0)])
    try:
        with staged.open('xb') as f:
            for frame in stream:
                if frame['frame']%30==0 and shutil.disk_usage(directory).free<RESERVE:
                    raise OSError('Free disk reserve exhausted during bake')
                block=frame['audio'];f.write(block);h.update(block);written+=len(block)
            f.flush();os.fsync(f.fileno())
        if written!=expected:raise RuntimeError('Incorrect baked PCM length')
        staged.rename(pcm)
        meta={'version':1,'identity':identity,'fps':t['fps'],'frames':t['frames'],
              'sample_rate':RATE,'channels':CHANNELS,'format':'interleaved f32le',
              'samples_per_channel':boundary(t,t['frames']),'bytes':written,
              'sha256':h.hexdigest(),'bake_seconds':time.perf_counter()-started}
        # Only the final manifest marks this artifact as complete.
        (directory/'manifest.json').write_text(json.dumps(meta,indent=2))
        return meta
    except BaseException:
        staged.unlink(missing_ok=True);pcm.unlink(missing_ok=True)
        raise
    finally:stream.close()

def bake(t,directory):
    directory=Path(directory);identity=timeline_identity(t)
    # Separate build workspace prevents a partially built graph being mistaken
    # for a completed artifact. Existing artifacts are never overwritten.
    graphdir=directory.parent/(directory.name+'-graph');graphdir.mkdir(parents=True,exist_ok=False)
    subset,p=adapter.domain_plan(t,0,t['frames'],'audio')
    adapter.build_audio_xml(subset,p,graphdir/'audio.xml');adapter.profile(t,graphdir/'profile')
    meta=_bake_graph(t,graphdir/'audio.xml',graphdir/'profile',directory,identity)
    try:
        if timeline_identity(t)!=identity:raise RuntimeError('Timeline sources changed during bake')
    except BaseException:
        (directory/'manifest.json').unlink(missing_ok=True)
        (directory/'audio.f32le').unlink(missing_ok=True)
        raise
    return meta

class FrozenAudio:
    """Verify the entire artifact once on open; range reads retain the open file."""
    def __init__(self,directory,expected_identity):
        directory=Path(directory);self.meta=json.loads((directory/'manifest.json').read_text())
        m=self.meta
        if m['version']!=1 or m['identity']!=expected_identity:
            raise ValueError('Snapshot does not match the fixed timeline')
        if (m['sample_rate'],m['channels'],m['format'])!=(RATE,CHANNELS,'interleaved f32le'):
            raise ValueError('Unsupported snapshot format')
        self.t={'fps':m['fps'],'frames':m['frames']}
        if m['bytes']!=boundary(self.t,m['frames'])*8:raise ValueError('Invalid snapshot length metadata')
        path=directory/'audio.f32le'
        if path.stat().st_size!=m['bytes'] or digest(path)!=m['sha256']:
            raise ValueError('Incomplete or corrupt snapshot')
        self.file=path.open('rb')
    def close(self):self.file.close()
    def __enter__(self):return self
    def __exit__(self,*exc):self.close()
    def frames(self,start,end):
        if not 0<=start<end<=self.t['frames']:raise ValueError('Invalid snapshot range')
        # No DSP reconstruction, decoding, or seek through earlier PCM.
        self.file.seek(boundary(self.t,start)*8)
        for i in range(start,end):
            begin=boundary(self.t,i);count=boundary(self.t,i+1)-begin
            pts=Fraction(begin,RATE)
            yield {'frame':i,'sample_start':begin,'samples':count,
                   'global_audio_pts':[pts.numerator,pts.denominator],
                   'audio':adapter.read_exact(self.file,count*8)}

def render(t,start,end,snapshot,directory):
    """Explicit opt-in preview; the default adapter is unchanged."""
    if snapshot.meta['identity']!=timeline_identity(t):
        raise ValueError('Timeline changed; explicitly bake a new snapshot')
    subset,p=adapter.domain_plan(t,start,end,'video');directory=Path(directory)
    directory.mkdir(parents=True,exist_ok=True)
    adapter.profile(t,directory/'profile');adapter.build_xml(subset,p,directory/'video.xml')
    (directory/'plan.json').write_text(json.dumps({'video':p,'audio':'explicit frozen PCM snapshot'},indent=2))
    audio=snapshot.frames(start,end)
    video=adapter.render_graphs(t,start,end,directory,[('video',directory/'video.xml',p['dependency'][0])])
    try:
        for v,a in zip(video,audio,strict=True):
            if (v['frame'],v['sample_start'],v['samples'])!=(a['frame'],a['sample_start'],a['samples']):
                raise RuntimeError('Snapshot/video alignment failure')
            yield {**v,'audio':a['audio']}
    finally:video.close();audio.close()

if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser();ap.add_argument('timeline',type=Path);ap.add_argument('artifact',type=Path)
    args=ap.parse_args();print(json.dumps(bake(json.loads(args.timeline.read_text()),args.artifact),indent=2))
