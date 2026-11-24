"""Render extracted blocks as a Markdown draft for full transcription."""
import re
import doc2struct


def markdown_styles(text):
    for tag, marker in [("strong", "**"), ("em", "*"), ("s", "~~")]:
        text = re.sub(f"<{tag}>(.*?)</{tag}>", lambda m: marker + m[1] + marker,
                      text, flags=re.DOTALL)
    return text


def block_markdown(row):
    if row["type"] == "list":
        styles = {(c["row"], c["col"]): c["runs"] for c in row.get("cell_runs") or []}
        lines = []
        for index, (depth, item) in enumerate(row.get("matrix") or []):
            value = {"id": 0, "type": "text", "text": item}
            if (index, 1) in styles:
                value["runs"] = styles[index, 1]
            rendered = doc2struct.convert_standardized_text_to_markdown([value], redundant="plain").strip()
            lines.append("  " * int(depth) + "- " + rendered)
        return markdown_styles("\n".join(lines))
    rendered = doc2struct.convert_standardized_text_to_markdown([row], redundant="plain").strip()
    if not rendered and row.get("text"):
        rendered = row["text"]
    return markdown_styles(rendered)



def render(rows, format="markdown"):
    if format == "edit":
        return render_edits(rows)
    if format != "markdown":
        raise ValueError(f"Unknown context format: {format}")
    return "\n\n".join(block_markdown(row) for row in rows)


def render_edits(rows):
    blocks = ['bN identifies an ORIGINAL source block. rN labels a table row, not a cell. Draft block types may be wrong; match the PDF.']
    for block in rows:
        if block['type'] != 'table':
            blocks.append(f"[b{block['id']} {block['type']}]\n" + block_markdown(block))
            continue
        matrix = block['matrix']
        widths = sorted({len(row) for row in matrix})
        lines = [f"[b{block['id']} table; rows r0..r{len(matrix)-1}; row widths {widths}]"]
        if block.get('preamble'):
            lines.append(block['preamble'])
        styles = {(s['row'],s['col']):s['runs'] for s in block.get('cell_runs') or []}
        for r, values in enumerate(matrix):
            cells = []
            for c, text in enumerate(values):
                value = {'type':'text','text':text}
                if (r,c) in styles:
                    value['runs'] = styles[r,c]
                cells.append(block_markdown(value).replace('\n','<br>').replace('|','\\|'))
            lines.append(f'r{r}: ' + ' | '.join(cells))
        for marker,text in block.get('footnotes') or []:
            lines.append(f'{marker} {text}')
        if block.get('postamble'):
            lines.append(block['postamble'])
        blocks.append('\n'.join(lines))
    return '\n\n'.join(blocks)
