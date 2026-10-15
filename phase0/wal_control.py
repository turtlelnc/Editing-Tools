import sqlite3,pathlib,shutil,json,time
R=pathlib.Path(__file__).parent/'results';rows=[]
for name,source in [('absolute','storage-100000-50.sqlite'),('chunk','chunk.sqlite')]:
 path=R/f'write-control-{name}.sqlite';shutil.copyfile(R/source,path);c=sqlite3.connect(path);c.executescript('PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA wal_autocheckpoint=0;');c.execute('PRAGMA wal_checkpoint(TRUNCATE)');t=time.perf_counter()
 if name=='absolute':
  c.execute('UPDATE item SET start=start+1');c.execute('UPDATE marker SET t=t+1')
 else:
  c.execute('UPDATE item SET rel=rel+1 WHERE b=0');c.execute('UPDATE marker SET rel=rel+1 WHERE b=0');c.execute('UPDATE block SET offset=offset+1 WHERE b>0')
 c.commit();rows.append({'layout':name,'all_50_track_ripple_ms':(time.perf_counter()-t)*1000,'new_wal_bytes':pathlib.Path(str(path)+'-wal').stat().st_size});c.close();path.unlink()
(R/'wal-control.json').write_text(json.dumps(rows,indent=2));print(rows)
