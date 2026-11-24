"""Compare selected local values from native and reference scoring passes."""
import json
from pathlib import Path
import sys
from debug_stages import ROOT,plain,differences
from parse_bench.evaluation.metrics.parse import table_next,forms_v2

job=json.loads((ROOT/'debug_input.jsonl').read_text(encoding='utf-8'))
name=sys.argv[1]
keys=sys.argv[2:]
snapshots=[]
def trace(frame,event,arg):
    if event=='return' and frame.f_code.co_name==name:
        snapshots.append({**{k:plain(v) for k,v in frame.f_locals.items() if k in keys},'__result':plain(arg)})
sys.setprofile(trace)
if job['metric']=='table_debug':table_next.score_table_next(job['reference'],job['prediction'],**job['flags'])
else:forms_v2.score_forms(forms_v2.forms_reference(job['metadata'],job['rules']),job['prediction'])
sys.setprofile(None)
native=[json.loads(s) for s in (ROOT/'function_dump.jsonl').read_text(encoding='utf-8').splitlines()]
print('passes',len(snapshots),len(native))
for i,(a,b) in enumerate(zip(snapshots,native)):
    print('pass',i)
    for k in keys:
        diff=list(differences(a.get(k),b.get(k)))
        if diff:print(k,repr(diff[:8])[:4000])
