"""Contracts that prevent unsupported modes from silently producing verified output."""
import sys,copy,json
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import adapter
p=adapter.P;t=json.loads((p/'fixture.json').read_text());results=[]
def reject(name,c,start=58,end=66):
 try:adapter.plan(c,start,end)
 except (adapter.UnsupportedEffect,ValueError) as e:results.append({'name':name,'rejected':True,'reason':str(e)})
 else:raise AssertionError(f'{name} was silently accepted')
c=copy.deepcopy(t);c['effects'].append({'id':'unknown','type':'minterpolate','anchor':0,'end':180});reject('unregistered-lookahead',c)
c=copy.deepcopy(t);c['clips'][0]['speed_curve']=[0,1,3,2];reject('variable-speed',c)
c=copy.deepcopy(t);c['clips'][0]['speed']=[-1,1];reject('reverse',c)
c=copy.deepcopy(t);c['effects'][0]['anchor']=15;reject('unverified-partial-scope',c)
c=copy.deepcopy(t);c['fps']=[30000,1001];c['frames']=13500;c['effects']=[];reject('ntsc-float-clock',c,13450,13460)
reject('empty-range',t,58,58)
c=copy.deepcopy(t);c['clips'].append({**c['clips'][0],'id':'unmapped-video','track':6});reject('unmapped-track',c)
c=copy.deepcopy(t);c['clips'][0]['track']=2;reject('wrong-track-role',c)
c=copy.deepcopy(t);c['clips'].append(copy.deepcopy(c['clips'][0]));reject('duplicate-id',c)
c=copy.deepcopy(t);c['width']=640;reject('unimplemented-geometry',c)
c=copy.deepcopy(t);c['crossfade']['end']=95;reject('missing-transition-handles',c)
# Serial temporal filters compose by sum, not max.
c=copy.deepcopy(t);c['effects']=[t['effects'][0],{**t['effects'][0],'id':'another','frames':3}]
assert adapter.plan(c,73,92)['dependency']==[67,92]
assert adapter.plan(t,129,136)['dependency']==[0,136]
(p/'results/contracts.json').write_text(json.dumps(results,indent=2));print(results)
