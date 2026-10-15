import sqlite3,time,json,random,os,resource,statistics,pathlib
root=pathlib.Path(__file__).parent/'results'; rng=random.Random(20261014)
def pct(xs,p): return sorted(xs)[min(len(xs)-1,int((len(xs)-1)*p))]
def bench(n,tracks):
 path=root/f'storage-{n}-{tracks}.sqlite'; path.unlink(missing_ok=True)
 c=sqlite3.connect(path); c.executescript('PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=1000; CREATE TABLE item(id INTEGER PRIMARY KEY,track INT,start INT,dur INT,media INT); CREATE INDEX ix ON item(track,start); CREATE TABLE keyframe(id INTEGER PRIMARY KEY,item INT,t INT,value REAL); CREATE TABLE marker(id INTEGER PRIMARY KEY,track INT,t INT); CREATE INDEX mx ON marker(track,t); CREATE TABLE oplog(rev INTEGER PRIMARY KEY,op TEXT,inverse TEXT);')
 c.executemany('INSERT INTO item VALUES(?,?,?,?,?)',((i,i%tracks,(i//tracks)*60,60,i%100) for i in range(n)))
 c.executemany('INSERT INTO keyframe VALUES(?,?,?,?)',((i,i//10,(i%10)*6,i%3/2) for i in range(n*10)))
 c.executemany('INSERT INTO marker VALUES(?,?,?)',((i,i%tracks,(i//tracks)*60) for i in range(n)))
 c.commit(); c.execute('PRAGMA wal_checkpoint(TRUNCATE)'); query=[]; edits=[]; undo=[]; batch=[]; writes=[]; walpeak=0; checkpoints=[]
 for rev in range(1000):
  tr=rng.randrange(tracks); at=rng.randrange(max(1,n//tracks))*60
  t=time.perf_counter(); list(c.execute('SELECT * FROM item WHERE track=? AND start<? AND start+dur>? LIMIT 100',(tr,at+600,at))); query.append((time.perf_counter()-t)*1000)
  before=os.path.getsize(str(path)+'-wal'); t=time.perf_counter()
  # Ripple moves clip starts, attached keyframes are local; track markers move too.
  selected=list(range(tracks)) if rev%10==0 else [tr]
  for track in selected:
   c.execute('UPDATE item SET start=start+1 WHERE track=? AND start>=?',(track,at)); c.execute('UPDATE marker SET t=t+1 WHERE track=? AND t>=?',(track,at))
  c.execute('INSERT INTO oplog VALUES(?,?,?)',(rev,json.dumps([selected,at,1]),json.dumps([selected,at+1,-1]))); c.commit(); edits.append((time.perf_counter()-t)*1000)
  after=os.path.getsize(str(path)+'-wal'); writes.append(max(0,after-before)); walpeak=max(walpeak,after)
  t=time.perf_counter()
  for track in selected:
   c.execute('UPDATE item SET start=start-1 WHERE track=? AND start>=?',(track,at+1)); c.execute('UPDATE marker SET t=t-1 WHERE track=? AND t>=?',(track,at+1))
  c.commit(); undo.append((time.perf_counter()-t)*1000)
  if rev%100==0:
   t=time.perf_counter(); c.execute('PRAGMA wal_checkpoint(PASSIVE)').fetchone(); checkpoints.append((time.perf_counter()-t)*1000)
   t=time.perf_counter()
   for j in range(100): c.execute('UPDATE item SET media=? WHERE id=?',(j%100,rng.randrange(n)))
   c.commit(); batch.append((time.perf_counter()-t)*1000)
 assert c.execute('SELECT COUNT(*) FROM item WHERE start!=(id / ?)*60',(tracks,)).fetchone()[0]==0
 result={'items':n,'tracks':tracks,'keyframes':n*10,'markers':n,'rounds':1000,'query_ms':{'p50':pct(query,.5),'p95':pct(query,.95),'p99':pct(query,.99)},'ripple_ms':{'p50':pct(edits,.5),'p99':pct(edits,.99)},'undo_ms':{'p50':pct(undo,.5),'p99':pct(undo,.99),'first100':statistics.mean(undo[:100]),'last100':statistics.mean(undo[-100:])},'batch100_ms':{'p95':pct(batch,.95)},'wal_peak_bytes':walpeak,'wal_file_growth_bytes_p99':pct(writes,.99),'checkpoint_ms_max':max(checkpoints),'db_bytes':path.stat().st_size,'oplog_bytes':c.execute('SELECT SUM(length(op)+length(inverse)) FROM oplog').fetchone()[0],'peak_process_rss_bytes':resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,'limitations':['WAL file growth is not write I/O; reused WAL pages are not counted','No product cache implementation; cache reuse not measured','100-op batch is parameter updates, not 100 ripples','Undo roundtrip immediately per loop, not long undo chain']}
 c.close(); print(json.dumps(result),flush=True); return result
if __name__=='__main__':
 results=[bench(10000,20),bench(100000,20),bench(100000,50)]
 (root/'storage.json').write_text(json.dumps(results,indent=2))
