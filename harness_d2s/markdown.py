"""Semantic HTML repair input converted into native standardized blocks."""
from html.parser import HTMLParser

class Tree(HTMLParser):
    def __init__(self, text):
        super().__init__(convert_charrefs=True)
        self.root={"tag":"root","children":[]};self.stack=[self.root];self.feed(text)
    def handle_starttag(self, tag, attrs):
        # Scope optional end tags to the nearest table, preserving nested tables.
        close = {'td': {'td','th'}, 'th': {'td','th'}, 'tr': {'tr'},
                 'thead': {'thead','tbody','tfoot'}, 'tbody': {'thead','tbody','tfoot'},
                 'tfoot': {'thead','tbody','tfoot'}}.get(tag)
        if close:
            for index in range(len(self.stack)-1,0,-1):
                if self.stack[index]['tag']=='table':break
                if self.stack[index]['tag'] in close:
                    del self.stack[index:]
                    break
        node={"tag":tag,"attrs":dict(attrs),"children":[]};self.stack[-1]["children"].append(node)
        if tag not in {'br','hr','img','meta','link','input'}:self.stack.append(node)
    def handle_endtag(self, tag):
        for index in range(len(self.stack)-1,0,-1):
            if self.stack[index]['tag']==tag:del self.stack[index:];break
    def handle_data(self,text):self.stack[-1]['children'].append(text)

FLAGS={'b':1,'strong':1,'i':2,'em':2,'u':4,'s':128,'del':128,'strike':128}
def runs(node, flags=0):
    if isinstance(node,str):return [{'text':node,'flags':flags}] if node else []
    flags|=FLAGS.get(node['tag'],0)
    css=dict(part.split(':',1) for part in node.get('attrs',{}).get('style','').lower().split(';') if ':' in part)
    css={key.strip():value.strip() for key,value in css.items()}
    weight=css.get('font-weight','')
    if weight in {'bold','bolder','600','700','800','900'}:flags|=1
    elif weight in {'normal','400'}:flags&=~1
    if css.get('font-style') in {'italic','oblique'}:flags|=2
    elif css.get('font-style')=='normal':flags&=~2
    decoration=css.get('text-decoration','')+' '+css.get('text-decoration-line','')
    if 'underline' in decoration:flags|=4
    if 'line-through' in decoration:flags|=128
    if node['tag']=='br':return [{'text':'\n','flags':flags}]
    return [part for child in node['children'] for part in runs(child,flags)]
def text(node):return ''.join(r['text'] for r in runs(node)).strip()
def descendants(node, tag):
    if isinstance(node,str):return []
    return ([node] if node['tag']==tag else [])+[n for child in node['children'] for n in descendants(child,tag)]
def styled(node, kind='text', level=0, prefix=''):
    parts=([{'text':prefix,'flags':0}] if prefix else [])+runs(node)
    return {'type':kind,'level':level,'text':''.join(r['text'] for r in parts),'runs':parts}

def convert_html(html, *, nested_lists=False):
    result=[]
    def walk(node):
        if isinstance(node,str):
            if node.strip():result.append({'type':'text','text':node.strip()})
            return
        tag=node['tag']
        if tag=='table':
            grid={};styles=[];height=width=0
            for r,tr in enumerate(descendants(node,'tr')):
                c=0
                for cell in [x for x in tr['children'] if isinstance(x,dict) and x['tag'] in {'td','th'}]:
                    while (r,c) in grid:c+=1
                    rs=max(1,int(cell['attrs'].get('rowspan','1')));cs=max(1,int(cell['attrs'].get('colspan','1')))
                    parts=runs(cell)
                    if parts:
                        parts[0]['text']=parts[0]['text'].lstrip()
                        parts[-1]['text']=parts[-1]['text'].rstrip()
                    value=''.join(part['text'] for part in parts)
                    for y in range(r,r+rs):
                        for x in range(c,c+cs):
                            grid[y,x]=value
                            if any(part['flags'] for part in parts):
                                styles.append({'row':y,'col':x,'runs':parts})
                    c+=cs;height=max(height,r+rs);width=max(width,c)
            matrix=[[grid.get((r,c),'') for c in range(width)] for r in range(height)]
            result.append({'type':'table','matrix':matrix,'cell_runs':styles})
        elif tag in {'h1','h2','h3','h4','h5','h6'}:result.append(styled(node,'title',int(tag[1])-1))
        elif tag in {'ol','ul'} and nested_lists:
            def emit_list(parent, depth=0):
                start=int(parent.get('attrs',{}).get('start','1'))
                items=[x for x in parent['children'] if isinstance(x,dict) and x['tag']=='li']
                for number,li in enumerate(items,start):
                    children=[x for x in li['children'] if not isinstance(x,dict) or x['tag'] not in {'ul','ol'}]
                    marker=f'{number}. ' if parent['tag']=='ol' else '- '
                    result.append(styled(dict(li,children=children),prefix='  '*depth+marker))
                    for child in li['children']:
                        if isinstance(child,dict) and child['tag'] in {'ul','ol'}:emit_list(child,depth+1)
            emit_list(node)
        elif tag in {'ol','ul'}:
            for number,li in enumerate([x for x in node['children'] if isinstance(x,dict) and x['tag']=='li'],int(node['attrs'].get('start','1'))):
                result.append(styled(li,prefix=f'{number}. ' if tag=='ol' else '- '))
        elif tag in {'p','pre','blockquote'}:result.append(styled(node))
        elif tag not in {'script','style','head'}:
            for child in node['children']:walk(child)
    walk(Tree(html).root)
    return result


"""Markdown repair tools returning native standardized rows."""
from markdown_it import MarkdownIt
from markdown_it.rules_block.table import escapedSplit
import re

PARSER = MarkdownIt("commonmark", {"html": True}).enable("table").enable("strikethrough")


def normalize_separators(markdown):
    """Repair only delimiter-row width; never alter supplied header/body cells."""
    lines = markdown.splitlines()
    # Respect code, HTML, and already recognized tables. A row of dashes in
    # an existing table is data and must never be mistaken for a delimiter.
    protected = [t.map for t in PARSER.parse(markdown)
                 if t.type in {'fence', 'code_block', 'html_block', 'table_open'} and t.map]
    for i in range(1, len(lines)):
        if any(a <= i < b for a, b in protected):
            continue
        header, separator = lines[i-1].strip(), lines[i].strip()
        if '|' not in header or '|' not in separator:
            continue
        def cells(line):
            values = escapedSplit(line)
            if line.startswith('|'):
                values.pop(0)
            if line.endswith('|') and not line.endswith('\\|'):
                values.pop()
            return values
        spec, width = cells(separator), len(cells(header))
        if width and spec and all(re.fullmatch(r':?-+:?', s.strip()) for s in spec) and len(spec) != width:
            lines[i] = '|' + '|'.join((spec + ['---'] * width)[:width]) + '|'
    return '\n'.join(lines)


def convert_markdown(markdown):
    markdown = normalize_separators(markdown)
    tokens = PARSER.parse(markdown)
    lines = markdown.splitlines()
    # GFM silently discards excess body cells. Widen the grid without dropping
    # values or changing any supplied cell's position.
    changed = False
    for token in tokens:
        if token.type == "table_open":
            start, end = token.map
            rows = []
            for number in range(start, end):
                line = lines[number].strip()
                cells = escapedSplit(line)
                if line.startswith("|"):
                    cells.pop(0)
                if line.endswith("|") and not line.endswith("\\|"):
                    cells.pop()
                rows.append(cells)
            width = max(map(len, rows))
            if width > len(rows[0]):
                changed = True
                for offset, cells in enumerate(rows):
                    fill = "---" if offset == 1 else ""
                    cells = cells + [fill] * (width - len(cells))
                    lines[start + offset] = "|" + "|".join(cell.replace("|", "\\|") for cell in cells) + "|"
    if changed:
        tokens = PARSER.parse("\n".join(lines))
    html = PARSER.renderer.render(tokens, PARSER.options, {})
    return convert_html(html, nested_lists=True)
