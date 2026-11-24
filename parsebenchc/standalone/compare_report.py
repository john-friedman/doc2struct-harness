"""Check headline scores, every lane, failures and weighted dimensions exactly."""
import argparse
from collections import defaultdict
import json
from pathlib import Path

PREFIXES=('text_next','formatting_v2','table_next','forms_v2')


def read_json(path):
    raw=path.read_bytes()
    for encoding in ('utf-8-sig','cp1252'):
        try:return json.loads(raw.decode(encoding))
        except UnicodeDecodeError:pass
    raise ValueError(f'Cannot decode {path}')


def metric_values(rows):
    return {m['metric_name']:m['value'] for m in rows if any(m['metric_name']==p or m['metric_name'].startswith(p+'_') for p in PREFIXES)}


def missing_reference(results_dir,ident):
    # Existing reports can omit a supplied result. Score that result afresh with
    # the unmodified Python implementation; never treat it as a native success
    # merely because no saved golden value exists.
    from parse_bench.evaluation.metrics.parse.next_metrics import compute_next_metrics
    from parse_bench.schemas.pipeline_io import InferenceResult
    from parse_bench.test_cases.loader import load_test_case
    for path in (results_dir/ident.split('/')[0]).glob('*.result.json'):
        raw=read_json(path)
        if raw['request']['example_id']!=ident:continue
        result=InferenceResult.model_validate(raw)
        case=load_test_case(results_dir/'references'/ident.split('/')[0]/Path(result.request.source_file_path).name,product_type_hint='parse')
        metrics=compute_next_metrics(result,case)
        return metric_values([m.model_dump(mode='json') for m in metrics]),None if metrics else 'No extended metric applies to this output'
    raise ValueError(f'Result not found: {ident}')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results_dir',type=Path)
    parser.add_argument('native_report',type=Path)
    args=parser.parse_args()
    expected={};errors={}
    for path in sorted(args.results_dir.glob('*/_evaluation_report.json')):
        for row in read_json(path)['per_example_results']:
            expected[row['test_id']]=metric_values(row['metrics'])
            if not row['success']:errors[row['test_id']]=row.get('error')
    native=read_json(args.native_report)
    differences=[];native_errors=[];seen=set();completed=[];matched_failures=0
    for row in native['per_example_results']:
        ident=row.get('test_id')
        if not ident:native_errors.append(row);continue
        seen.add(ident)
        if ident not in expected:
            expected[ident],error=missing_reference(args.results_dir,ident)
            if error:errors[ident]=error
            completed.append(ident)
        if 'error' in row:
            if row['error']==errors.get(ident):matched_failures+=1
            else:native_errors.append(row)
        elif ident in errors:
            differences.append({'id':ident,'expected_error':errors[ident],'actual':row})
        elif row['metrics']!=expected[ident]:
            differences.append({'id':ident,'expected':expected[ident],'actual':row['metrics']})
    from parse_bench.evaluation.next_score import NEXT_DIMENSIONS
    by_subset=defaultdict(list)
    for ident in seen:by_subset[ident.split('/')[0]].append(ident)
    dimensions={}
    for dimension,slots in NEXT_DIMENSIONS.items():
        total=0.;missing=False
        for slot in slots:
            ids=by_subset[slot.subset]
            values=[expected[i][slot.metric] for i in ids if i not in errors and slot.metric in expected[i]]
            failures=sum(i in errors and not str(errors[i]).startswith(('Worker error:','Evaluation error:','Task execution error:')) for i in ids)
            if not values:missing=True;break
            total+=slot.weight*(sum(values)/(len(values)+failures))
        dimensions[dimension]=None if missing else total/sum(slot.weight for slot in slots)
    aggregate_differences={k:{'expected':v,'actual':native['dimensions'].get(k)} for k,v in dimensions.items() if native['dimensions'].get(k)!=v}
    report={'documents':native['documents'],'seconds':native['seconds'],'dimensions':native['dimensions'],'compared':len(seen),'exact_matches':len(seen)-len(differences)-len(native_errors),'matched_failures':matched_failures,'fresh_reference_scores':completed,'differences':differences,'aggregate_differences':aggregate_differences,'native_errors':native_errors,'reference_errors':{i:errors[i] for i in seen if i in errors},'unmatched_reference_ids':sorted(set(expected)-seen)}
    output=args.native_report.with_suffix('.comparison.json')
    output.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ['differences','native_errors','reference_errors','unmatched_reference_ids']},indent=2))
    for item in differences[:5]+native_errors[:5]:print(json.dumps(item,ensure_ascii=False))
    print(output)
    return bool(differences or native_errors or aggregate_differences)


if __name__=='__main__':raise SystemExit(main())
