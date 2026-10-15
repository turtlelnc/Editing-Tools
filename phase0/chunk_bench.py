import sqlite3,pathlib,time,random,json,resource
R=pathlib.Path(__file__).parent/'results';p=R/'chunk.sqlite';p.unlink(missing_ok=True);c=sqlite3.connect(p);c.executescript('PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE block(track INT,b INT,offset INT,PRIMARY KEY(track,b)); CREATE TABLE item(id INT PRIMARY KEY,track INT,b INT,rel INT,dur INT,media INT); CREATE INDEX ix ON item(track,b,rel); CREATE TABLE marker(id INT PRIMARY KEY,track INT,b INT,rel INT); CREATE INDEX mx ON marker(track,b,rel); CREATE TABLE keyframe(id INT PRIMARY KEY,item INT,t INT,value REAL); CREATE TABLE oplog(rev INT PRIMARY KEY,op TEXT,inverse TEXT);')
n=100000;tracks=50;B=256
c.executemany('INSERT INTO block VALUES(?,?,?)',((tr,b,b*B*60) for tr in range(tracks) for b in range((n//tracks+B-1)//B)))
c.executemany('INSERT INTO item VALUES(?,?,?,?,?,?)',((i,i%tracks,(i//tracks)//B,((i//tracks)%B)*60,60,i%100) for i in range(n)))
c.executemany('INSERT INTO marker VALUES(?,?,?,?)',((i,i%tracks,(i//tracks)//B,((i//tracks)%B)*60) for i in range(n)))
c.executemany('INSERT INTO keyframe VALUES(?,?,?,?)',((i,i//10,i%10*6,i%3/2) for i in range(n*10)));c.commit()
rng=random.Random(20261014);q=[];edit=[];undo=[]
def move(ts,at,d):
 b=at//(B*60);rel=at-b*B*60
 for tr in ts:
  for table in ['item','marker']:c.execute(f'UPDATE {table} SET rel=rel+? WHERE track=? AND b=? AND rel>=?',(d,tr,b,rel))
  c.execute('UPDATE block SET offset=offset+? WHERE track=? AND b>?',(d,tr,b))
for rev in range(1000):
 tr=rng.randrange(tracks);at=rng.randrange(n//tracks)*60;ts=list(range(tracks)) if rev%10==0 else [tr]
 t=time.perf_counter();list(c.execute('SELECT i.* FROM item i JOIN block b ON i.track=b.track AND i.b=b.b WHERE i.track=? AND i.rel+b.offset<? AND i.rel+b.offset+i.dur>? LIMIT 100',(tr,at+600,at)));q.append((time.perf_counter()-t)*1000)
 t=time.perf_counter();move(ts,at,1);c.execute('INSERT INTO oplog VALUES(?,?,?)',(rev,json.dumps([ts,at,1]),json.dumps([ts,at+1,-1])));c.commit();edit.append((time.perf_counter()-t)*1000)
 t=time.perf_counter();move(ts,at+1,-1);c.commit();undo.append((time.perf_counter()-t)*1000)
 if rev%100==0:
  for j in range(100):c.execute('UPDATE item SET media=? WHERE id=?',(j%100,rng.randrange(n)))
  c.commit()
assert c.execute('SELECT COUNT(*) FROM item i JOIN block b ON i.track=b.track AND i.b=b.b WHERE i.rel+b.offset!=(i.id / ?)*60',(tracks,)).fetchone()[0]==0
pct=lambda xs,p:sorted(xs)[int((len(xs)-1)*p)]
x={'items':n,'tracks':tracks,'keyframes':n*10,'markers':n,'rounds':1000,'block_size':B,'query_p95_ms':pct(q,.95),'ripple_p99_ms':pct(edit,.99),'undo_p99_ms':pct(undo,.99),'peak_rss_bytes':resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,'limitations':['Static blocks; insertion/split/rebalance not exercised','Same seed and operation draws as indexed 50-track baseline','Cache correctness and real disk write bytes not measured']};(R/'chunk.json').write_text(json.dumps(x,indent=2));print(json.dumps(x))
