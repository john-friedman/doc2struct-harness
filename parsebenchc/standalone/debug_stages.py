"""Compare native parser stages with the frozen scorer during development."""
from dataclasses import asdict, is_dataclass
import json
from pathlib import Path
import subprocess
import sys
from parse_bench.evaluation.metrics.parse import table_next,forms_v2

ROOT=Path(__file__).resolve().parents[1]/'build/standalone'
RESULTS=Path('runs/luna_low_full/20261119T082154Z/score')

def plain(value):
    if is_dataclass(value):return plain(asdict(value))
    if isinstance(value,dict):return {str(k):plain(v) for k,v in value.items()}
    if isinstance(value,(list,tuple,set,frozenset)):return [plain(v) for v in value]
    return value

def differences(a,b,path=''):
    if isinstance(a,dict) and isinstance(b,dict):
        for k in a.keys()|b.keys():
            if k not in a or k not in b:yield path+'/'+k,a.get(k),b.get(k)
            else:yield from differences(a[k],b[k],path+'/'+k)
    elif isinstance(a,list) and isinstance(b,list):
        if len(a)!=len(b):yield path+'/length',len(a),len(b)
        for i,(x,y) in enumerate(zip(a,b)):yield from differences(x,y,path+'/'+str(i))
    elif a!=b:yield path,a,b

def check(module,function,args,kw=None):
    expected=getattr(module,function)(*args,**(kw or {}))
    job={'module':module.__name__.rsplit('.',1)[-1],'function':function,'args':plain(args),'kwargs':plain(kw or {})}
    (ROOT/'stages_input.jsonl').write_text(json.dumps(job,ensure_ascii=False)+'\n',encoding='utf-8')
    subprocess.run([str(ROOT/'parsebenchc.exe'),'pairs',str(ROOT/'stages_input.jsonl'),str(ROOT/'stages_output.jsonl')],check=True,stderr=subprocess.DEVNULL)
    actual=json.loads((ROOT/'stages_output.jsonl').read_text(encoding='utf-8'))['value']
    diff=list(differences(plain(expected),actual))
    print(function,len(diff),'differences',repr(diff[:8])[:3000])
    (ROOT/'stages_expected.json').write_text(json.dumps(plain(expected),ensure_ascii=False,indent=2),encoding='utf-8')
    (ROOT/'stages_actual.json').write_text(json.dumps(actual,ensure_ascii=False,indent=2),encoding='utf-8')
    return expected

def main():
    ident=sys.argv[1]
    for path in (RESULTS/ident.split('/')[0]).glob('*.result.json'):
        data=json.loads(path.read_bytes())
        if data['request']['example_id']==ident:break
    md=data['output']['markdown']
    if ident.startswith('forms/'):
        case=json.loads((RESULTS/'references'/f'{ident}.test.json').read_bytes())
        ref=check(forms_v2,'forms_reference',[case.get('metadata',{}),case.get('test_rules',[])])
        heads=[tuple(''.join(forms_v2.word_keys(ref.items[a].label)) if a>=0 else '' for a in t.captions) for t in ref.tables]
        shared=frozenset(h for h in heads if heads.count(h)>1)
        out=check(forms_v2,'read_output',[md,shared,frozenset(heads)])
        job={'metric':'forms_locate','metadata':case.get('metadata',{}),'rules':case.get('test_rules',[]),'prediction':md}
        (ROOT/'stages_input.jsonl').write_text(json.dumps(job,ensure_ascii=False)+'\n',encoding='utf-8')
        subprocess.run([str(ROOT/'parsebenchc.exe'),'pairs',str(ROOT/'stages_input.jsonl'),str(ROOT/'stages_output.jsonl')],check=True)
        actual=json.loads((ROOT/'stages_output.jsonl').read_text(encoding='utf-8'))['value']
        print('locate',list(differences(plain(forms_v2.locate(ref,out)),actual))[:12])
    else:
        trip=[]
        for side,md in [('reference',json.loads((RESULTS/'references'/f'{ident}.test.json').read_bytes())['expected_markdown']),('prediction',md)]:
            print(side)
            tables=check(table_next,'parse_tables',[md])
            for t in tables:
                tg=check(table_next,'clean_grid',[t])
                schema=check(table_next,'infer_schema',[tg.g,1])
                trip.append(check(table_next,'build_triples',[tg,schema]).triples)
        pairs={}
        for a in trip[0]:
            for b in trip[1]:
                if table_next.nx_digits(a.v)==table_next.nx_digits(b.v):pairs[a.v,b.v]=table_next.value_sim(a.v,b.v)
        jobs=[{'module':'table_next','function':'value_sim','args':list(p)} for p in pairs]
        (ROOT/'stages_input.jsonl').write_text(''.join(json.dumps(j,ensure_ascii=False)+'\n' for j in jobs),encoding='utf-8')
        subprocess.run([str(ROOT/'parsebenchc.exe'),'pairs',str(ROOT/'stages_input.jsonl'),str(ROOT/'stages_output.jsonl'),'8'],check=True)
        actual=[json.loads(s)['value'] for s in (ROOT/'stages_output.jsonl').read_text(encoding='utf-8').splitlines()]
        print('value_sim diffs',[(p,e,a) for (p,e),a in zip(pairs.items(),actual) if e!=a][:15])

if __name__=='__main__':main()
