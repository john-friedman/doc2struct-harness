"""Differential tests run the independent executable against unmodified Python."""
from dataclasses import asdict
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

from parse_bench.evaluation.metrics.parse import text_next as text,formatting_v2 as fmt,table_next as table,forms_v2 as forms

ROOT=Path(__file__).resolve().parents[1]
EXE=ROOT/'build/standalone'/('parsebenchc.exe' if sys.platform=='win32' else 'parsebenchc')


def plain(value):
    return json.loads(json.dumps(value,ensure_ascii=False))


class NativeScoring(unittest.TestCase):
    def run_jobs(self,jobs,workers=4,exit_code=0):
        with tempfile.TemporaryDirectory(prefix='parity-東京-',dir=ROOT/'build') as directory:
            base=Path(directory)/('long-'+80*'x')
            base.mkdir()
            source=base/'inputs.jsonl';target=base/'outputs.jsonl'
            source.write_text(''.join(json.dumps(j,ensure_ascii=False)+'\n' for j in jobs),encoding='utf-8')
            proc=subprocess.run([str(EXE),'pairs',str(source),str(target),str(workers)],capture_output=True,text=True,encoding='utf-8')
            self.assertEqual(proc.returncode,exit_code,proc.stderr)
            values=[json.loads(s) for s in target.read_text(encoding='utf-8').splitlines()]
            self.assertEqual(len(values),len(jobs))
            return values

    def check_functions(self,module,jobs):
        expected=[plain(getattr(module,fn)(*args)) for fn,args in jobs]
        native=self.run_jobs([{'module':module.__name__.rsplit('.',1)[-1],'function':fn,'args':args} for fn,args in jobs])
        self.assertEqual([v['value'] for v in native],expected)

    def test_alignment_ties_unicode_and_bitset_word_boundaries(self):
        rng=random.Random(8142);alphabet=['a','b','c','','é','e\u0301','中文','🐈']
        cases=[]
        for i in range(500):
            a=rng.choices(alphabet,k=rng.randrange(160));b=rng.choices(alphabet,k=rng.randrange(160))
            cases.append(('_lcs_len',[a,b]))
            if i<100:cases.append(('_lcs_pairs',[a[:35],b[:35]]))
        self.check_functions(text,cases)

    def test_weighted_form_alignment_and_one_edit(self):
        rng=random.Random(991);alphabet=['amount','amout','amnout','école','ecole','abcde','abcxef','a','2','金额','☑']
        jobs=[]
        for i in range(250):
            phrase=rng.choices(alphabet,k=rng.randrange(1,20));hyp=rng.choices(alphabet,k=rng.randrange(30))
            jobs.append(('_window_hits',[phrase,[forms._weight(w) for w in phrase],hyp]))
        self.check_functions(forms,jobs)

    def test_large_gap_alignment_uses_rapidfuzz_ties_and_slices(self):
        rng=random.Random(781);alphabet=['a','b','c','猫','é']
        cases=[('_large_lcs_pairs',[rng.choices(alphabet,k=rng.randrange(2,130)),rng.choices(alphabet,k=rng.randrange(2,130))]) for _ in range(80)]
        cases.append(('_lcs_pairs',[rng.choices(alphabet,k=2100),rng.choices(alphabet,k=2100)]))
        cases.append(('_large_lcs_pairs',[['a']*20001,['b']+['a']*19999+['c']]))
        self.check_functions(text,cases)

    def test_numeric_markers_units_footnotes_and_unicode_labels(self):
        values=['','-','None','n/a','$ -','123','124','00123','1,230','1.230','1 230','123%','123¹','123<sup>1</sup>','(123)','$123','€123','YES','yes','✓','[x]','p̂2','$\\hat{p}2','50%','50']
        self.check_functions(table,[('value_sim',[a,b,'rate %']) for a in values for b in values])
        self.check_functions(table,[('_relabelled',['p̂2','$\\hat{p}2']),('_relabelled',['p̂xhispanic','$\\hat{p} \\times$ hispanic'])])

    def test_table_structure_assignment_and_alternate_readings(self):
        rng=random.Random(155);jobs=[];expected=[]
        for i in range(55):
            rows=[['Year','Alpha','Beta']]+[[str(2020+j),str(rng.randrange(100)),str(rng.randrange(100))] for j in range(rng.randrange(2,9))]
            predicted=[r[:] for r in rows]
            if i%3==0:
                body=predicted[1:];rng.shuffle(body);predicted[1:]=body
            if i%3==1:predicted[-1][-1]=str(int(predicted[-1][-1])+1)
            if i%5==0:predicted.append(predicted[-1][:])
            def html(rs):return '<table>'+''.join('<tr>'+''.join('<td>'+v+'</td>' for v in row)+'</tr>' for row in rs)+'</table>'
            ref=html(rows);hyp=html(predicted)
            if i%4==0:hyp='\n'.join('| '+' | '.join(r)+' |' for r in [predicted[0],['---']*3,*predicted[1:]])
            jobs.append({'metric':'tables','reference':ref,'prediction':hyp})
            score=table.score_table_next(ref,hyp)
            expected.append({'table_next':score.score,'table_next_content':score.content,'table_next_row_order':score.row_order})
        self.assertEqual(self.run_jobs(jobs),expected)

    def test_nested_and_malformed_table_boundaries(self):
        cases=['é<table><tr><td>猫<table><td>x</table></td></table>🐈','<table><tr><th>a<table><td>b','<table><tr></tr><table>x</table>','<!-- <table>x --> <TABLE><TH>x</TH></TABLE>','<table><tr><td>a</td><table>b</table>','<tablex>ignored</table><table>real']
        self.check_functions(table,[('_table_spans',[s]) for s in cases])

    def test_text_and_formatting_policy(self):
        cases=[('## Total\n\n**Revenue:** ૧૨૩ kg','## Total\n\nRevenue: ૧૨૪ kg'),('10<sup>-3</sup> and H<sub>2</sub>O','10-3 and H2O'),('<u>103</u>~~101~~','103 101'),('**中文** <i>école</i> ภาษาไทย','中文 école ภาษาไทย'),('☑ Approved\n\n[ ] Rejected','☐ Approved\n\n[x] Rejected'),('Price: €1,234.50\nDate: 2025-01-02','Price: €1,234.50\nDate: 2025-01-03'),('```\n**code**\n```\n\nA *word*.','```\n**code**\n```\n\nA word.')]
        jobs=[];expected=[]
        for ref,hyp in cases:
            t=text.score_text_next(ref,hyp);f=fmt.score_formatting(ref,hyp)
            jobs.append({'metric':'text','reference':ref,'prediction':hyp})
            expected.append({'text_next':t.score,**{'text_next_'+k:getattr(t,k) for k in ('words','critical','integrity','order') if getattr(t,k) is not None}})
            jobs.append({'metric':'formatting','reference':ref,'prediction':hyp})
            expected.append({'formatting_v2':f.score,**{'formatting_v2_'+k:v for k,v in f.lanes.items() if v is not None}})
        self.assertEqual(self.run_jobs(jobs),expected)

    def test_forms_blank_date_and_field_readings(self):
        rules=[{'type':'form_field','label':label,'value':value,'value_type':'text','page_number':1} for label,value in [('Name','Alice'),('Date',None),('DD',None),('MM',None),('AAAA',None),('Amount','4,930')]]
        jobs=[];expected=[]
        for hyp in ['Name: Alice\nDate: ______\nAmount: 4,930','Name: Alice\nDate: 12/03/2025\nAmount: 4,930','Name: Alicia\nAmount: 2  $4,930']:
            ref=forms.forms_reference({},rules);s=forms.score_forms(ref,hyp)
            jobs.append({'metric':'forms','metadata':{},'rules':rules,'prediction':hyp})
            expected.append({'forms_v2':s.score,**{'forms_v2_'+k:v for k,v in s.lanes.items() if v is not None}})
        self.assertEqual(self.run_jobs(jobs),expected)

    def test_worker_failure_isolation_and_utf8_long_paths(self):
        result=self.run_jobs([{'function':'does_not_exist','args':[]},{'module':'text_next','function':'_lcs_len','args':[['a'],['a']]}],exit_code=1)
        self.assertIn('error',result[0]);self.assertEqual(result[1],{'value':1})

    def test_unsupported_dimensions_are_explicit(self):
        jobs=[{'testcase':{'test_rules':[{'type':kind}]},'result':{'output':{'markdown':'hello'}}} for kind in ('layout','chart_data_point')]
        for row in self.run_jobs(jobs,exit_code=1):self.assertIn('outside this local',row['error'])


if __name__=='__main__':unittest.main()
