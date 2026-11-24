"""Measure the complete C command, including discovery, input and report output."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results_dir',type=Path)
    parser.add_argument('references_dir',type=Path)
    parser.add_argument('--workers',nargs='+',type=int,default=[24])
    parser.add_argument('--repeat',type=int,default=3)
    parser.add_argument('--executable',type=Path,default=root/'build/standalone'/('parsebenchc.exe' if sys.platform=='win32' else 'parsebenchc'))
    parser.add_argument('--output',type=Path,default=root/'build/native_benchmark.json')
    args=parser.parse_args()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    reports=args.output.parent/(args.output.stem+'_runs')
    reports.mkdir(parents=True,exist_ok=True)
    rows=[]
    for workers in args.workers:
        for trial in range(args.repeat):
            output=reports/f'native_{workers}_{trial}.json'
            command=[str(args.executable),'score',str(args.results_dir),str(args.references_dir),str(output),str(workers)]
            started=time.perf_counter();proc=subprocess.run(command,capture_output=True,text=True,encoding='utf-8');wall=time.perf_counter()-started
            if proc.returncode not in (0,1):raise RuntimeError(proc.stderr)
            report=json.loads(output.read_text(encoding='utf-8'))
            errors=[r for r in report['per_example_results'] if r.get('error') and r['error']!='No extended metric applies to this output']
            if errors:raise RuntimeError(errors)
            row={'workers':workers,'trial':trial,'wall_seconds':wall,'scoring_seconds':report['seconds'],'documents':report['documents'],'matched_unscorable_outputs':report['failures'],'report':str(output)}
            rows.append(row);print(json.dumps(row),flush=True)
    medians={str(w):statistics.median(r['wall_seconds'] for r in rows if r['workers']==w) for w in args.workers}
    args.output.write_text(json.dumps({'scope':'complete native command; no Python scoring; includes filesystem discovery, JSON input and JSON output','results_dir':str(args.results_dir.resolve()),'executable':str(args.executable.resolve()),'median_wall_seconds':medians,'runs':rows},indent=2),encoding='utf-8')
    print(args.output)


if __name__=='__main__':main()
