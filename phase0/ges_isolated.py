# Repeat the 1000-clip run with no competing GES process and distinct artifacts.
import runpy
x=runpy.run_path('phase0/ges_bench.py',run_name='definitions_only') if False else None
from pathlib import Path
s=Path('phase0/ges_bench.py').read_text();s=s[:s.index("run('ges-save-one'")].replace("R/'ges.json'","R/'ges-isolated.json'")
exec(s)
run('ges-render-1000-isolated',base+['-l',str(R/'ges-1000.xges'),'-o',str(M/'ges-1000-isolated.mkv'),'-f','video/x-matroska:video/x-ffv,ffvversion=1'],timeout=180)
(R/'ges-isolated.json').write_text(__import__('json').dumps(rows,indent=2))
