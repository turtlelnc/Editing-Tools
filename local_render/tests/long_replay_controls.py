"""Separate audio-graph and state-activation controls after serial timing trials."""
import sys,json,xml.etree.ElementTree as ET
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent));import long_replay as bench
P=bench.P;R=P/'results/long-replay/30min';bench.R=R
summary=json.loads((R/'summary.json').read_text());reference={int(k):v for k,v in json.loads((R/'full-reference-1800/hashes.json').read_text()).items()}
start=summary['trials'][-1]['range'][0][0];window=[(start,start+30)];end=start+30
root=ET.Element('mlt',producer='audio-main')
for id,filename in [('audio','audio.wav'),('silence','silence.wav')]:
 node=ET.SubElement(root,'producer',id=id);bench.adapter.props(node,{'mlt_service':'avformat','resource':str(P/'media'/filename),'video_index':-1,'audio_index':0,'threads':1})
loop=ET.SubElement(root,'playlist',id='audio-loop')
for i in range((end+179)//180):
 for id,offset in [('audio',0),('silence',90)]:
  length=min(90,end-i*180-offset)
  if length>0:ET.SubElement(loop,'entry',producer=id,**{'in':'0','out':str(length-1)})
main=ET.SubElement(root,'tractor',id='audio-main',**{'in':'0','out':str(end-1)})
ET.SubElement(main,'track',producer='audio-loop')
fx=ET.SubElement(main,'filter',**{'in':'0','out':str(end-1)})
bench.adapter.props(fx,{'mlt_service':'sox','effect':'reverb 70 50 100 100 10 0'})
xml=R/'audio-only.xml';ET.indent(root);ET.ElementTree(root).write(xml,encoding='utf-8',xml_declaration=True);bench.adapter.profile(bench.base,xml.with_suffix('.profile'))
audio,stats=bench.execute('independent-audio-1800',xml,0,end,4,window,300)
stats['audio_exact']=all(v['audio']==reference[k]['audio'] and v['sample_start']==reference[k]['sample_start'] and v['samples']==reference[k]['samples'] for k,v in audio.items())
stats['video_comparison']='Not applicable: audio-only graph emits diagnostic black frames'
assert stats['audio_exact']
cold,coldstats=bench.execute('cold-history-control',R/'graph-1800.xml',start-4,end,4,window,30)
coldstats['video_exact']=all(v['video']==reference[k]['video'] for k,v in cold.items());coldstats['audio_exact']=all(v['audio']==reference[k]['audio'] for k,v in cold.items())
assert coldstats['video_exact'] and not coldstats['audio_exact'],'Missing stateful-history negative control'
(R/'controls.json').write_text(json.dumps({'independent_audio':stats,'short_history_negative_control':coldstats},indent=2))
