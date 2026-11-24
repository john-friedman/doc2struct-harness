"""Build corrected documents from changed content and lossless source copies."""
from copy import deepcopy
import re

from . import Tool
from .grid import submit_grid
from ..document import Document

STYLE_BITS = {'bold':1,'italic':2,'underline':4,'strikeout':128}


def source_id(value):
    if not isinstance(value, str) or not re.fullmatch(r'b[0-9]+', value):
        raise ValueError('Source block IDs must be displayed bN addresses')
    return int(value[1:])


def literal(block):
    temporary = Document([])
    submit_grid(temporary, blocks=[block])
    return temporary.rows


def styled_runs(runs, text, style):
    result = deepcopy(runs) if runs else [{'text':text,'flags':0}]
    for run in result:
        for name,enabled in style.items():
            bit = STYLE_BITS[name]
            run['flags'] = (run.get('flags',0) | bit) if enabled else (run.get('flags',0) & ~bit)
    return result



def modified_copy(spec, source, table=False):
    allowed = {'ref','style','style_cols','cells'} if table else {'ref','style','heading'}
    if set(spec)-allowed or 'ref' not in spec:
        raise ValueError('Unknown modified-reference fields')
    ref = spec['ref']
    pattern = r'b[0-9]+:r([0-9]+)(?:\.\.r([0-9]+))?' if table else r'b[0-9]+(?:\.\.b[0-9]+)?'
    match = re.fullmatch(pattern,ref) if isinstance(ref,str) else None
    if not match:
        raise ValueError('Use an original block reference or, inside tables, a row reference')
    copied = replacement_blocks([{'table':[ref]}] if table else [ref],source)
    if 'heading' in spec:
        level = spec['heading']
        for block in copied:
            if block['type']=='list' and len(block['matrix'])==1:
                runs = next((c['runs'] for c in block.get('cell_runs') or [] if c['row']==0 and c['col']==1),[])
                block.update(text=block['matrix'][0][1],runs=deepcopy(runs),matrix=None,cell_runs=None)
        if type(level) is not int or not 0<=level<=6 or any(b.get('matrix') for b in copied):
            raise ValueError('heading requires text blocks and level 0..6')
        for block in copied:block.update(type='title' if level else 'text',level=max(0,level-1))
    style = spec.get('style',{})
    if not isinstance(style,dict) or set(style)-STYLE_BITS.keys() or any(type(v) is not bool for v in style.values()):
        raise ValueError('style accepts bold, italic, underline and strikeout booleans')
    if 'style_cols' in spec and not style:
        raise ValueError('style_cols selects cells for a style change')
    for block in copied:
        if not style:
            continue
        if block.get('matrix'):
            matrix = block['matrix']
            cols = spec.get('style_cols',range(len(matrix[0])))
            if not cols or any(type(c) is not int or not 0<=c<len(matrix[0]) for c in cols) or len(set(cols))!=len(cols):
                raise ValueError('Style columns must be unique valid original column indices')
            cells = {(s['row'],s['col']):s['runs'] for s in block.get('cell_runs') or []}
            for r,row in enumerate(matrix):
                for c in cols:
                    if row[c]:cells[r,c] = styled_runs(cells.get((r,c)),row[c],style)
            block['cell_runs'] = [{'row':r,'col':c,'runs':runs} for (r,c),runs in sorted(cells.items())]
        else:
            block['runs'] = styled_runs(block.get('runs'),block.get('text') or '',style)
    if table and 'cells' in spec:
        block = copied[0]
        matrix = block['matrix']
        styles = {(s['row'],s['col']):s['runs'] for s in block.get('cell_runs') or []}
        first = int(match[1])
        seen = set()
        for patch in spec['cells']:
            if set(patch)!={'row','col','markdown'}:
                raise ValueError('Cell corrections need original row, col and markdown')
            r,c = patch['row'],patch['col']
            if (type(r) is not int or type(c) is not int or not 0<=r-first<len(matrix)
                or not 0<=c<len(matrix[r-first]) or (r,c) in seen):
                raise ValueError('Cell corrections need unique original coordinates inside the copied rows')
            parsed = literal({'table':[[patch['markdown']]]})[0]
            matrix[r-first][c] = parsed['matrix'][0][0]
            styles.pop((r-first,c),None)
            if parsed['cell_runs']:styles[r-first,c] = parsed['cell_runs'][0]['runs']
            seen.add((r,c))
        block['cell_runs'] = [{'row':r,'col':c,'runs':runs} for (r,c),runs in sorted(styles.items())]
    return copied


def replacement_blocks(blocks, source):
    rows = []
    for block in blocks:
        if isinstance(block,dict) and 'ref' in block:
            rows.extend(modified_copy(block,source))
            continue
        if isinstance(block,str):
            match = re.fullmatch(r'b([0-9]+)(?:\.\.b([0-9]+))?',block)
            if not match:
                raise ValueError('Block references use bN or bN..bM')
            block = {'copy_blocks':{'first_block':'b'+match[1],'last_block':'b'+(match[2] or match[1])}}
        if set(block) == {'copy_blocks'}:
            copy = block['copy_blocks']
            if set(copy) != {'first_block','last_block'}:
                raise ValueError('copy_blocks needs first_block and last_block')
            ids = list(source)
            first, last = source_id(copy['first_block']), source_id(copy['last_block'])
            if first not in source or last not in source or ids.index(first) > ids.index(last):
                raise ValueError('Copied blocks are outside the original source')
            rows.extend(deepcopy(source[i]) for i in ids[ids.index(first):ids.index(last)+1])
        elif set(block) == {'markdown'}:
            rows.extend(literal(block))
        elif set(block) == {'table'}:
            matrix, styles = [], []
            for spec in block['table']:
                if isinstance(spec,dict) and 'ref' in spec:
                    parsed = modified_copy(spec,source,table=True)[0]
                    offset = len(matrix)
                    styles.extend(dict(s,row=s['row']+offset) for s in parsed.get('cell_runs') or [])
                    matrix.extend(parsed['matrix'])
                    continue
                if isinstance(spec,str):
                    match = re.fullmatch(r'b([0-9]+):r([0-9]+)(?:\.\.r([0-9]+))?',spec)
                    if not match:
                        raise ValueError('Row references use bN:rA or bN:rA..rB')
                    spec = {'copy_rows':{'block_id':'b'+match[1],'first_row':int(match[2]),'last_row':int(match[3] or match[2])}}
                if isinstance(spec, list):
                    parsed = literal({'table':[spec]})[0]
                    values, runs = parsed['matrix'], parsed['cell_runs']
                elif isinstance(spec, dict) and set(spec) == {'copy_rows'}:
                    copy = spec['copy_rows']
                    if set(copy) != {'block_id','first_row','last_row'}:
                        raise ValueError('copy_rows needs block_id, first_row and last_row')
                    original = source.get(source_id(copy['block_id']))
                    first, last = copy['first_row'], copy['last_row']
                    if (original is None or original['type'] != 'table' or
                        type(first) is not int or type(last) is not int or
                        not 0 <= first <= last < len(original['matrix'])):
                        raise ValueError('Copied rows are outside the original source table')
                    values = deepcopy(original['matrix'][first:last+1])
                    runs = [dict(deepcopy(s), row=s['row']-first) for s in original.get('cell_runs') or []
                            if first <= s['row'] <= last]
                else:
                    raise ValueError('A table row must be a cell array or copy_rows object')
                offset = len(matrix)
                styles.extend(dict(s, row=s['row']+offset) for s in runs)
                matrix.extend(values)
            if not matrix or not any(matrix):
                raise ValueError('Replacement tables must contain cells')
            width = max(map(len,matrix))
            rows.append({'type':'table','matrix':[r+['']*(width-len(r)) for r in matrix], 'cell_runs':styles})
        else:
            raise ValueError('Replacement blocks contain exactly markdown or table')
    return rows


class EditTool(Tool):
    def __call__(self, document, blocks):
        source = {b['id']:b for b in document.rows}
        replacement = replacement_blocks(blocks, source)
        return document.execute('replace_blocks', {'ids':list(source),'blocks':replacement})


def obj(properties):
    return {'type':'object','properties':properties,'required':list(properties),'additionalProperties':False}


BLOCK_REF = {'type':'string','pattern':r'^b[0-9]+(?:\.\.b[0-9]+)?$'}
ROW_REF = {'type':'string','pattern':r'^b[0-9]+:r[0-9]+(?:\.\.r[0-9]+)?$'}
STYLE = {'type':'object','properties':{name:{'type':'boolean'} for name in STYLE_BITS},'additionalProperties':False}
BLOCK_EDIT = obj({'ref':BLOCK_REF})
BLOCK_EDIT['properties'].update(style=STYLE,heading={'type':'integer','minimum':0,'maximum':6})
ROW_EDIT = obj({'ref':ROW_REF})
ROW_EDIT['properties'].update(style=STYLE,style_cols={'type':'array','items':{'type':'integer','minimum':0},'minItems':1,'uniqueItems':True},
    cells={'type':'array','items':obj({'row':{'type':'integer','minimum':0},'col':{'type':'integer','minimum':0},'markdown':{'type':'string'}})})
BLOCK = {'oneOf':[
    BLOCK_REF,
    BLOCK_EDIT,
    obj({'markdown':{'type':'string'}}),
    obj({'table':{'type':'array','items':{'oneOf':[{'type':'array','items':{'type':'string'}},ROW_REF,ROW_EDIT]}}}),
]}
apply_edits = EditTool('apply_edits',
    'Return the complete corrected document in order. Strings copy ORIGINAL blocks ("b1..b4") or, '
    'inside tables, rows ("b5:r3..r20"). Ranges are inclusive; row/column indices start at zero. '
    'Omitted content is deleted. Use markdown/table for new text or rebuilt layouts. '
    'A modified copy {ref,...} can set heading (0=prose, 1..6=heading), or style flags without changing other styles. '
    'For copied table rows, style_cols limits styling to those columns; cells corrects ORIGINAL row/col values with Markdown, overriding styles. '
    'Example: {"blocks":[{"ref":"b2","heading":2},{"table":[{"ref":"b5:r3..r20","style":{"bold":true},"style_cols":[0]}]}]}',
    obj({'blocks':{'type':'array','items':BLOCK}}))
