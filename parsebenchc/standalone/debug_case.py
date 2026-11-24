"""Dump comparable native/Python internals for a single regression case."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from parse_bench.schemas.pipeline_io import InferenceResult
from parse_bench.test_cases.loader import load_test_case
from parse_bench.evaluation.metrics.parse import text_next,table_next,forms_v2


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('results_dir',type=Path)
    parser.add_argument('test_id')
    parser.add_argument('mode',choices=['table_debug','forms_debug','tokenize'])
    args=parser.parse_args()
    result=None
    subset=args.test_id.split('/')[0]
    for path in (args.results_dir/subset).glob('*.result.json'):
        obj=json.loads(path.read_bytes())
        if obj['request']['example_id']==args.test_id:
            result=InferenceResult.model_validate(obj);break
    if result is None:raise ValueError(args.test_id)
    source=args.results_dir/'references'/subset/Path(result.request.source_file_path).name
    case=load_test_case(source,product_type_hint='parse')
    reference=text_next.reference_markdown(case.expected_markdown,case.test_rules) or ''
    prediction=text_next.output_hypothesis(result.output)
    job={'reference':reference,'prediction':prediction,'metric':args.mode,'id':args.test_id}
    if args.mode=='table_debug':
        flags={key:getattr(case,key) for key in ['max_top_title_rows','allow_splitting_ambiguous_merged_tables','trm_unsupported','allow_merged_sub_rows']}
        job['flags']=flags;expected=asdict(table_next.score_table_next(reference,prediction,**flags))
    elif args.mode=='forms_debug':
        job['metadata']=case.metadata;job['rules']=[r.model_dump(mode='json') for r in case.test_rules or []]
        ref=forms_v2.forms_reference(case.metadata,case.test_rules);expected=asdict(forms_v2.score_forms(ref,prediction))
    else:
        expected={name:[[asdict(t) for t in text_next.tokenize(md)[0]],text_next.tokenize(md)[1]] for name,md in [('reference',reference),('prediction',prediction)]}
    root=Path(__file__).resolve().parents[1]/'build/standalone'
    (root/'debug_input.jsonl').write_text(json.dumps(job,ensure_ascii=False)+'\n',encoding='utf-8')
    subprocess.run([str(root/'parsebenchc.exe'),'pairs',str(root/'debug_input.jsonl'),str(root/'debug_native.jsonl')],check=True)
    actual=json.loads((root/'debug_native.jsonl').read_text(encoding='utf-8'))
    (root/'debug_expected.json').write_text(json.dumps(expected,ensure_ascii=False,indent=2),encoding='utf-8')
    (root/'debug_actual.json').write_text(json.dumps(actual,ensure_ascii=False,indent=2),encoding='utf-8')
    if args.mode=='tokenize':
        for side in ('reference','prediction'):
            a,b=expected[side][0],actual[side][0]
            print(side,len(a),len(b))
            for i,(left,right) in enumerate(zip(a,b)):
                if left!=right:print('First token difference',i,left,right);break
    else:
        for key in expected:
            if expected[key]!=actual.get(key):print(key, '\nPython:',str(expected[key])[:6000],'\nNative:',str(actual.get(key))[:6000])


if __name__=='__main__':main()
