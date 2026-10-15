"""Minimal MLT local render adapter. No MCP, cache, or serialized DSP state.

The public exact policy preserves absolute positions, creates a fresh graph,
and eagerly replays the dependency interval. Experimental policies intentionally
omit history to measure failure. Streams are unencoded RGBA + stereo f32 PCM.
"""
from __future__ import annotations
from pathlib import Path
from fractions import Fraction
import json,math,os,struct,subprocess,xml.etree.ElementTree as ET

P=Path(__file__).resolve().parent
ROOT=P.parent
HEADER=struct.Struct('<6Q')
class UnsupportedEffect(ValueError):pass

def validate_layout(timeline:dict):
    """Reject inputs the fixed-layout XML writer would otherwise ignore."""
    if (timeline['width'],timeline['height'])!=(320,180):
        raise UnsupportedEffect('Only the verified 320x180 geometry is implemented')
    n,d=timeline['fps']
    if not isinstance(n,int) or not isinstance(d,int) or n<=0 or d<=0:
        raise ValueError('Frame rate must be a positive integer rational')
    roles={'a':(1,'video'),'b':(2,'video'),'overlay':(3,'video'),
           'cue1':(4,'subtitle'),'cue2':(4,'subtitle'),
           'audio':(5,'audio'),'tail_padding':(5,'audio')}
    ids=set();tracks={}
    for c in timeline['clips']:
        if c['id'] in ids:raise ValueError('Duplicate clip id')
        ids.add(c['id'])
        if roles.get(c['id'])!=(c['track'],c['kind']):
            raise UnsupportedEffect('Clip id, track or kind is outside the verified fixed layout')
        if not 0<=c['start']<c['end']<=timeline['frames'] or c['src_in']<0:
            raise ValueError('Invalid clip/source interval')
        tracks.setdefault(c['track'],[]).append(c)
    for clips in tracks.values():
        ordered=sorted(clips,key=lambda c:c['start'])
        if any(a['end']>b['start'] for a,b in zip(ordered,ordered[1:])):
            raise ValueError('Overlapping clips on same track')
    cross=timeline['crossfade'];clips={c['id']:c for c in timeline['clips']}
    if cross['a']!='a' or cross['b']!='b':
        raise UnsupportedEffect('Only the explicit a/b crossfade is implemented')
    if 'b' in clips and (cross['start']!=clips['b']['start'] or not cross['start']<cross['end']<clips['b']['end']):
        raise UnsupportedEffect('Crossfade must begin at B and end within its interval')
    if 'a' in clips and 'b' in clips and clips['a']['end']!=cross['end']:
        raise UnsupportedEffect('A/B transition requires the declared overlapping handles')

def plan(timeline:dict, start:int, end:int, policy='exact', warmup:int=0)->dict:
    validate_layout(timeline)
    if not 0<=start<end<=timeline['frames']:raise ValueError('Require half-open integer frame range within timeline')
    if policy not in ['exact','bounded','cold','fixed']:raise ValueError('Unknown policy')
    if warmup<0:raise ValueError('Negative warm-up')
    history=0;anchors=[];decisions=[]
    for fx in timeline.get('effects',[]):
        if fx['anchor']!=0 or fx['end']!=timeline['frames']:
            raise UnsupportedEffect('Only full-timeline root effect scopes have been verified')
        if fx['type']=='tmix':
            n=int(fx['frames'])
            if not 1<=n<=1024:raise ValueError('Invalid tmix window')
            history+=n-1
            decisions.append({'effect':fx['id'],'state':'bounded','history_frames':n-1})
        elif fx['type']=='sox.reverb':
            # Feedback reverb has no proven finite bit-exact history bound.
            anchors.append(fx['anchor']);decisions.append({'effect':fx['id'],'state':'unbounded','replay_anchor':fx['anchor']})
        else:raise UnsupportedEffect(f"Effect {fx['type']} has no verified dependency contract")
    begin=max(0,start-history)
    if policy=='exact' and anchors:begin=min([begin]+anchors)
    elif policy=='cold':begin=start
    elif policy=='fixed':begin=max(0,start-warmup)
    n,d=timeline['fps']
    fps32=struct.unpack('<f',struct.pack('<f',n/d))[0]
    for frame in range(begin,end+1):
        native=int(frame*48000/fps32+0.5)
        rational=(frame*48000*d*2+n)//(2*n)
        if native!=rational:
            raise UnsupportedEffect(f'MLT float-rate audio clock differs from rational clock at frame {frame}: {native} vs {rational}')
    selected=[c for c in timeline['clips'] if c['start']<end and c['end']>begin]
    sources=[]
    for c in selected:
        if any(k in c for k in ['speed_curve','reverse','source_effects']):
            raise UnsupportedEffect('Variable/reverse speed and source-scoped effects have no verified contract')
        speed=Fraction(*c['speed'])
        if speed<=0:raise UnsupportedEffect('Reverse/variable speed is not implemented')
        lo=max(begin,c['start'])-c['start']+c['src_in']
        hi=min(end,c['end'])-c['start']+c['src_in']
        sources.append({'clip':c['id'],'timewarp_input_frames':[lo,hi],
          'source_profile_frame_dependency':[math.floor(lo*speed),math.ceil(hi*speed)+1],
          'source_position_timebase':timeline['fps'],
          'note':'conservative +1 for timewarp rounding; decoder may additionally seek from an earlier keyframe'})
    return {'output':[start,end],'dependency':[begin,end],'warmup_frames':start-begin,
      'policy':policy,'exact_contract':policy=='exact','effects':decisions,
      'selected_clips':[c['id'] for c in selected],'source_dependencies':sources,
      'position_policy':'absolute frames and original effect/transition scopes; no rebasing'}

def props(node,values):
    for k,v in values.items():ET.SubElement(node,'property',name=k).text=str(v)

def build_xml(t:dict,p:dict,path:Path,assets:Path= P/'media'):
    root=ET.Element('mlt',producer='main');N=t['frames'];selected=set(p['selected_clips'])
    bg=ET.SubElement(root,'producer',id='bg',**{'in':'0','out':str(N-1)})
    props(bg,{'mlt_service':'color','resource':'black','length':N,'eof':'pause'})
    clips={c['id']:c for c in t['clips']}
    for c in t['clips']:
        if c['id'] not in selected:continue
        pathasset=str((assets/c['asset']).resolve());speed=Fraction(*c['speed'])
        node=ET.SubElement(root,'producer',id=c['id'])
        values={'mlt_service':'avformat' if speed==1 else 'timewarp',
          'resource':pathasset if speed==1 else f'{float(speed)}:{pathasset}',
          'audio_index':0 if c['kind']=='audio' else -1,
          'video_index':-1 if c['kind']=='audio' else 0,'threads':1,'eof':'pause'}
        props(node,values)
    pl=ET.SubElement(root,'playlist',id='track0');ET.SubElement(pl,'entry',producer='bg',**{'in':'0','out':str(N-1)})
    for tr in range(1,6):
        pl=ET.SubElement(root,'playlist',id=f'track{tr}');cursor=0
        for c in sorted((c for c in t['clips'] if c['track']==tr and c['id'] in selected),key=lambda c:c['start']):
            if c['start']<cursor:raise ValueError('Overlapping clips on same track')
            if c['start']>cursor:ET.SubElement(pl,'blank',length=str(c['start']-cursor))
            ET.SubElement(pl,'entry',producer=c['id'],**{'in':str(c['src_in']),'out':str(c['src_in']+c['end']-c['start']-1)})
            cursor=c['end']
        if cursor<N:ET.SubElement(pl,'blank',length=str(N-cursor))
    main=ET.SubElement(root,'tractor',id='main',**{'in':'0','out':str(N-1)})
    mt=ET.SubElement(main,'multitrack')
    for tr in range(6):
        ET.SubElement(mt,'track',producer=f'track{tr}',hide='video' if tr==5 else 'audio')
    def trans(service,tr,start,end,extra):
        node=ET.SubElement(main,'transition',**{'in':str(start),'out':str(end-1)})
        props(node,{'mlt_service':service,'a_track':0,'b_track':tr,'progressive':1,**extra})
    if 'a' in selected:trans('composite',1,clips['a']['start'],clips['a']['end'],{'geometry':'0/0:320x180:100','fill':1})
    cross=t['crossfade']
    if 'b' in selected:
        trans('composite' if cross.get('disabled') else 'luma',2,cross['start'],cross['end'],{'geometry':'0/0:320x180:100','alpha_over':1})
        trans('composite',2,cross['end'],clips['b']['end'],{'geometry':'0/0:320x180:100','fill':1})
    if 'overlay' in selected:
        c=clips['overlay'];trans('composite',3,c['start'],c['end'],{'geometry':'0=0/0:320x180:40;124=0/0:320x180:80','fill':1})
    for id in ['cue1','cue2']:
        if id in selected:
            c=clips[id];trans('composite',4,c['start'],c['end'],{'geometry':'0/0:320x180:100','fill':1})
    if any(c['id'] in selected and c['kind']=='audio' for c in t['clips']):trans('mix',5,0,N,{'sum':1,'always_active':1})
    for fx in t.get('effects',[]):
        node=ET.SubElement(main,'filter',**{'in':str(fx['anchor']),'out':str(fx['end']-1)})
        if fx['type']=='tmix':props(node,{'mlt_service':'avfilter.tmix','av.frames':fx['frames'],'av.threads':1,'position':'frame'})
        elif fx['type']=='sox.reverb':props(node,{'mlt_service':'sox','effect':fx['options']})
    ET.indent(root);ET.ElementTree(root).write(path,encoding='utf-8',xml_declaration=True)

def environment():
    if os.environ.get('MLT_SYSTEM')=='1':
        return dict(os.environ)
    return {**os.environ,'MLT_REPOSITORY':str(P/'runtime-modules'),
      'MLT_DATA':str(ROOT/'phase0/runtime/mlt/7.42.0/share/mlt'),
      'MLT_LOG_LEVEL':'error'}

def profile(t:dict,path:Path):
    n,d=t['fps'];path.write_text(f'description=LocalRender\nframe_rate_num={n}\nframe_rate_den={d}\nwidth={t["width"]}\nheight={t["height"]}\nprogressive=1\nsample_aspect_num=1\nsample_aspect_den=1\ndisplay_aspect_num=16\ndisplay_aspect_den=9\ncolorspace=709\n')

def read_exact(f,n):
    out=bytearray()
    while len(out)<n:
        b=f.read(n-len(out))
        if not b:raise RuntimeError(f'Truncated MLT stream: {len(out)} of {n} bytes')
        out+=b
    return bytes(out)

def render(t:dict,start:int,end:int,directory:Path,policy='exact',warmup=0):
    directory.mkdir(parents=True,exist_ok=True);p=plan(t,start,end,policy,warmup)
    (directory/'plan.json').write_text(json.dumps(p,indent=2))
    build_xml(t,p,directory/'timeline.xml');profile(t,directory/'profile')
    cmd=[str(P/'mlt_pull'),str(directory/'timeline.xml'),str(directory/'profile'),str(p['dependency'][0]),str(start),str(end)]
    (directory/'command.json').write_text(json.dumps(cmd,indent=2))
    with (directory/'stderr.log').open('w') as log:
        proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=log,env=environment())
        try:
            for i in range(start,end):
                head=HEADER.unpack(read_exact(proc.stdout,HEADER.size));f,actual,original,sample_start,samples,vbytes=head
                video=read_exact(proc.stdout,vbytes);audio=read_exact(proc.stdout,samples*2*4)
                video_pts=Fraction(f*t['fps'][1],t['fps'][0]);audio_pts=Fraction(sample_start,48000)
                yield {'global_video_pts':[video_pts.numerator,video_pts.denominator],
                  'global_audio_pts':[audio_pts.numerator,audio_pts.denominator],
                  'frame':f,'mlt_position':actual,'source_position':original,'sample_start':sample_start,'samples':samples,'video':video,'audio':audio}
            if proc.stdout.read(1):raise RuntimeError('Unexpected extra output')
            if proc.wait(timeout=10):raise RuntimeError(f'MLT pull failed: see {directory}/stderr.log')
            errors=(directory/'stderr.log').read_text()
            if any(x in errors.lower() for x in ['failed','cannot','not found','error','unexpected']):raise RuntimeError(f'MLT diagnostic failure: {errors[:400]}')
        finally:
            proc.stdout.close()
            if proc.poll() is None:proc.kill();proc.wait()

if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser();ap.add_argument('timeline',type=Path);ap.add_argument('start',type=int);ap.add_argument('end',type=int);ap.add_argument('output',type=Path)
    args=ap.parse_args();t=json.loads(args.timeline.read_text());args.output.mkdir(parents=True,exist_ok=True)
    stamps=[]
    with (args.output/'video.rgba').open('wb') as v,(args.output/'audio.f32le').open('wb') as a:
        for f in render(t,args.start,args.end,args.output):
            v.write(f.pop('video'));a.write(f.pop('audio'));stamps.append(f)
    (args.output/'timestamps.json').write_text(json.dumps({'fps':t['fps'],'sample_rate':48000,'channels':2,'format':'rgba + interleaved f32le','frames':stamps},indent=2))
