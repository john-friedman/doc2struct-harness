"""Section edits with explicit prose patches and table column projection."""
from copy import deepcopy
import re

from . import Tool
from ..context import block_markdown
from .edits import (modified_copy, replacement_blocks, literal, obj,
                    BLOCK_REF, ROW_REF, STYLE, ROW_EDIT)


def prose(spec,source):
    if set(spec)-{'ref','heading','style','join','replace'}:
        raise ValueError('Unknown prose edit options')
    rows=replacement_blocks([spec['ref']],source)
    markdown=[block_markdown(r) for r in rows]
    changed=set()
    if 'join' in spec:
        separator=spec['join']
        if separator not in {'',' ','\n','\n\n'} or any(r['type']=='table' for r in rows):
            raise ValueError('join combines prose blocks using an explicit whitespace separator')
        markdown=[separator.join(re.sub(r'^#{1,6} +','',text) if row['type']=='title' else text for row,text in zip(rows,markdown))]
        rows=[rows[0]];changed.add(0)
    for patch in spec.get('replace',[]):
        if not isinstance(patch,list) or len(patch)!=2 or not all(isinstance(v,str) for v in patch) or not patch[0]:
            raise ValueError('replace entries are [exact original text, replacement Markdown] pairs')
        old,new=patch
        index=next((i for i,text in enumerate(markdown) if old in text),None)
        if index is None:
            raise ValueError('Replacement Markdown not found; patches apply in order after join')
        markdown[index]=markdown[index].replace(old,new,1);changed.add(index)
    rows=[block for i,row in enumerate(rows) for block in (literal({'markdown':markdown[i]}) if i in changed else [row])]
    options={k:v for k,v in spec.items() if k in {'heading','style'}}
    if options and rows:
        temporary={i:dict(row,id=i) for i,row in enumerate(rows)}
        rows=modified_copy(dict(options,ref=f'b0..b{len(rows)-1}'),temporary)
    return rows


def columns(row,selectors):
    matrix=row['matrix'];n=len(matrix);width=len(matrix[0]);expanded=[]
    for selector in selectors:
        if isinstance(selector,str) and re.fullmatch(r'\d+\.\.\d+',selector):
            first,last=map(int,selector.split('..'))
            if first>last:raise ValueError('Column ranges must be ascending')
            expanded.extend(range(first,last+1))
        else:expanded.append(selector)
    styles={(s['row'],s['col']):s['runs'] for s in row.get('cell_runs') or []}
    result=[[] for _ in matrix];new_styles=[]
    for col,selector in enumerate(expanded):
        if isinstance(selector,dict):
            if set(selector)!={'values'} or not isinstance(selector['values'],list) or len(selector['values'])!=n or not all(isinstance(v,str) for v in selector['values']):
                raise ValueError('A literal column needs exactly one Markdown value per copied row')
            parsed=literal({'table':[[v] for v in selector['values']]})[0]
            for r,line in enumerate(parsed['matrix']):result[r].append(line[0])
            new_styles.extend(dict(s,col=col) for s in parsed['cell_runs'])
            continue
        indices=selector if isinstance(selector,list) else [selector]
        if not indices or any(type(c) is not int or not 0<=c<width for c in indices):
            raise ValueError('Column selectors must address ORIGINAL columns inside the copied table')
        for r,line in enumerate(matrix):
            parts=[]
            for c in indices:
                if not line[c]:continue
                if parts:parts.append({'text':' ','flags':0})
                parts.extend(deepcopy(styles.get((r,c)) or [{'text':line[c],'flags':0}]))
            result[r].append(''.join(p['text'] for p in parts))
            if any(p.get('flags') for p in parts):new_styles.append({'row':r,'col':col,'runs':parts})
    if not expanded:raise ValueError('cols must select at least one column')
    return dict(row,matrix=result,cell_runs=new_styles)


def build(blocks,source):
    rows=[]
    for block in blocks:
        if isinstance(block,dict) and 'ref' in block:
            rows.extend(prose(block,source));continue
        if not isinstance(block,dict) or set(block)!={'table'}:
            rows.extend(replacement_blocks([block],source));continue
        matrix=[];styles=[]
        for spec in block['table']:
            if isinstance(spec,dict) and 'ref' in spec:
                spec=deepcopy(spec)
                if 'cells' in spec:
                    patches=[]
                    for patch in spec['cells']:
                        if isinstance(patch,list):
                            if len(patch)!=3:raise ValueError('Cell patches are [original row, original col, Markdown]')
                            patch=dict(zip(['row','col','markdown'],patch))
                        patches.append(patch)
                    spec['cells']=patches
                table=modified_copy({k:v for k,v in spec.items() if k!='cols'},source,table=True)[0]
                if 'cols' in spec:table=columns(table,spec['cols'])
            else:table=replacement_blocks([{'table':[spec]}],source)[0]
            offset=len(matrix);matrix.extend(table['matrix'])
            styles.extend(dict(s,row=s['row']+offset) for s in table.get('cell_runs') or [])
        if not matrix:raise ValueError('Tables must contain rows')
        width=max(map(len,matrix))
        rows.append({'type':'table','matrix':[r+['']*(width-len(r)) for r in matrix],'cell_runs':styles})
    return rows


class CompactEdits(Tool):
    def __call__(self,document,blocks):
        rows=build(blocks,{b['id']:b for b in document.rows})
        return document.execute('replace_blocks',{'ids':[b['id'] for b in document.rows],'blocks':rows})


TEXT_EDIT=obj({'ref':BLOCK_REF})
TEXT_EDIT['properties'].update(heading={'type':'integer','minimum':0,'maximum':6},style=STYLE,
    join={'type':'string','enum':['',' ','\n','\n\n']},
    replace={'type':'array','items':{'type':'array','items':{'type':'string'},'minItems':2,'maxItems':2}})
TABLE_EDIT=deepcopy(ROW_EDIT)
TABLE_EDIT['properties']['cells']['items']={'oneOf':[TABLE_EDIT['properties']['cells']['items'],
    {'type':'array','items':{'oneOf':[{'type':'integer'},{'type':'string'}]},'minItems':3,'maxItems':3}]}
TABLE_EDIT['properties']['cols']={'type':'array','minItems':1,'items':{'oneOf':[
    {'type':'integer','minimum':0},{'type':'string','pattern':r'^\d+\.\.\d+$'},
    {'type':'array','items':{'type':'integer','minimum':0},'minItems':1},
    obj({'values':{'type':'array','items':{'type':'string'}}})]}}
BLOCK={'oneOf':[BLOCK_REF,TEXT_EDIT,obj({'markdown':{'type':'string'}}),
    obj({'table':{'type':'array','items':{'oneOf':[{'type':'array','items':{'type':'string'}},ROW_REF,TABLE_EDIT]}}})]}
apply_edits=CompactEdits('apply_edits',
    'Return all assigned content in order. Strings copy ORIGINAL blocks (b1..b4), or rows inside tables (b5:r0..r9). '
    'Omitted content is deleted. A {ref} copy keeps original headings and styles; heading=0 makes prose, 1..6 headings; style sets bold/italic/underline/strikeout booleans. '
    'For prose, join concatenates copied blocks with that exact whitespace; replace is [[old Markdown, new Markdown],...], each replacing the first EXACT occurrence in the displayed ORIGINAL Markdown, in order after join. Heading/style overrides apply LAST. Unchanged Markdown keeps its styles. '
    'For table row copies, cells uses [ORIGINAL row, ORIGINAL col, Markdown] triples; style_cols selects ORIGINAL columns for style changes. Then cols projects columns: an index, "0..3" range, [0,1] joins cells with spaces, or {values:[...]} supplies one Markdown cell per copied row. All indices start at zero. '
    'Use literal markdown/table for additions or rebuilt layouts.',obj({'blocks':{'type':'array','items':BLOCK}}))
