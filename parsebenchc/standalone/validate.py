"""Prepare real inputs, run the independent C executable, compare Python scores."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from parsebenchc.tools.profile_local import samples
from parse_bench.evaluation.metrics.parse import text_next,formatting_v2,table_next,forms_v2


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('results_dir')
    parser.add_argument('--per-subset',type=int,default=2)
    parser.add_argument('--metric',choices=['text','formatting','tables','forms'],required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]/'build/standalone'
    jobs=[];expected=[];ids=[]
    for result,case in samples(args.results_dir,args.per_subset):
        reference=text_next.reference_markdown(case.expected_markdown,case.test_rules)
        prediction=text_next.output_hypothesis(result.output)
        job={'reference':reference or '', 'prediction':prediction, 'metric':args.metric}
        if args.metric=='text':
            if not reference or not text_next.text_next_applies(reference,case.test_rules,case.metadata):continue
            score=text_next.score_text_next(reference,prediction)
            value={'text_next':score.score,**{'text_next_'+key:getattr(score,key) for key in ['words','critical','integrity','order'] if getattr(score,key) is not None}}
        elif args.metric=='formatting':
            if not reference:continue
            score=formatting_v2.score_formatting(reference,prediction)
            value={'formatting_v2':score.score,**{'formatting_v2_'+key:val for key,val in score.lanes.items() if val is not None}}
        elif args.metric=='tables':
            if not table_next.has_reference_tables(reference):continue
            flags={key:getattr(case,key) for key in ['max_top_title_rows','allow_splitting_ambiguous_merged_tables','trm_unsupported','allow_merged_sub_rows']}
            job['flags']=flags
            score=table_next.score_table_next(reference,prediction,**flags)
            value={'table_next':score.score,'table_next_content':score.content,'table_next_row_order':score.row_order}
        else:
            ref=forms_v2.forms_reference(case.metadata,case.test_rules)
            if not forms_v2.forms_v2_applies(ref):continue
            job['metadata']=case.metadata
            job['rules']=[x.model_dump(mode='json') if hasattr(x,'model_dump') else x for x in case.test_rules or []]
            score=forms_v2.score_forms(ref,prediction)
            value={'forms_v2':score.score,**{'forms_v2_'+key:val for key,val in score.lanes.items() if val is not None}}
        jobs.append(job);expected.append(value);ids.append(case.test_id)
    infile=root/(args.metric+'_inputs.jsonl');outfile=root/(args.metric+'_outputs.jsonl')
    infile.write_text(''.join(json.dumps(j,ensure_ascii=False)+'\n' for j in jobs),encoding='utf-8')
    start=time.perf_counter()
    result=subprocess.run([str(root/'parsebenchc.exe'),'pairs',str(infile),str(outfile)],capture_output=True,text=True,encoding='utf-8')
    print(result.stderr)
    if result.returncode:raise SystemExit(result.returncode)
    actual=[json.loads(line) for line in outfile.read_text(encoding='utf-8').splitlines()]
    assert len(actual)==len(expected)
    differences=[{'id':ident,'expected':e,'actual':a} for ident,e,a in zip(ids,expected,actual) if e!=a]
    (root/(args.metric+'_comparison.json')).write_text(json.dumps({'documents':len(jobs),'seconds':time.perf_counter()-start,'differences':differences},ensure_ascii=False,indent=2),encoding='utf-8')
    print('Compared',len(jobs),'documents;',len(differences),'mismatches')
    for diff in differences[:3]:print(json.dumps(diff,ensure_ascii=False))
    if differences:raise SystemExit(1)


if __name__=='__main__':main()
