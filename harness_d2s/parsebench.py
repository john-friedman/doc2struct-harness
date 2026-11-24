"""Convert standardized rows to ParseBench's parse-output schema.

Conversion uses only parser output. References, test rules, subset names, and
expected answers never enter this module. Tables are HTML so the official
scorer receives the actual grid, without inventing header rows or cell spans.
"""
from html import escape
from datetime import datetime, timezone
from markdown_it import MarkdownIt

import doc2struct


PIPELINE = "doc2struct-harness"
_MARKDOWN = MarkdownIt("commonmark", {"html": True})


def _ordered_list(self, tokens, index, options, env):
    # Browser-generated list counters are invisible to text-only consumers.
    # Emit their original labels explicitly and suppress the visual counters.
    return '<ol style="list-style-type:none">\n'


def _list_item(self, tokens, index, options, env):
    token = tokens[index]
    marker = escape(token.info + token.markup) + " " if token.info else ""
    return "<li>" + marker


_MARKDOWN.add_render_rule("ordered_list_open", _ordered_list)
_MARKDOWN.add_render_rule("list_item_open", _list_item)


def paragraph(text):
    return "<p>" + escape(text).replace("\n", "<br>\n") + "</p>" if text else ""


def cell_html(text, runs):
    if not runs:
        return escape(text or "")
    parts = []
    for run in runs:
        tags = [tag for bit, tag in ((1, "strong"), (2, "em"), (4, "u"), (128, "s"))
                if (run.get("flags") or 0) & bit]
        parts.append("".join(f"<{tag}>" for tag in tags)
                     + escape(run.get("text", ""))
                     + "".join(f"</{tag}>" for tag in reversed(tags)))
    return "".join(parts)


def table_html(row, current_title):
    blocks = []
    if row.get("text") and row["text"] != current_title:
        blocks.append(paragraph(row["text"]))
    blocks.append(paragraph(row.get("preamble")))
    styles = {(cell["row"], cell["col"]): cell["runs"]
              for cell in (row.get("cell_runs") or [])}
    table = ["<table>", "<tbody>"]
    for r, cells in enumerate(row.get("matrix") or []):
        table.append("<tr>" + "".join(
            "<td>" + cell_html(value, styles.get((r, c))) + "</td>"
            for c, value in enumerate(cells)) + "</tr>")
    table.extend(["</tbody>", "</table>"])
    blocks.append("\n".join(table))
    for marker, text in row.get("footnotes") or []:
        blocks.append(paragraph(" ".join(part for part in (marker, text) if part)))
    blocks.append(paragraph(row.get("postamble")))
    return "\n\n".join(block for block in blocks if block)


def convert(rows, *, document_id, source_file_path="", pipeline_name=PIPELINE):
    chunks, pending = [], []
    current_title = None

    def flush():
        if pending:
            text = doc2struct.convert_standardized_text_to_markdown(pending, redundant="plain")
            if text.strip():
                # Project valid Markdown escapes to their visible HTML form.
                # The benchmark's token scanner otherwise reads backslashes
                # inside literal URLs as part of the address. This is ordinary
                # Markdown rendering, independent of references and case IDs.
                chunks.append(_MARKDOWN.render(text).strip())
            pending.clear()

    for row in rows:
        # Arrow materializes absent optional run flags as null; native default is 0.
        if row.get("runs"):
            row = dict(row, runs=[dict(run, flags=run.get("flags") or 0) for run in row["runs"]])
            if not row.get("text"):
                row["text"] = "".join(run["text"] for run in row["runs"])
        if row.get("type") == "title":
            current_title = row.get("text")
        if row.get("type") == "table":
            flush()
            chunks.append(table_html(row, current_title))
        else:
            pending.append(row)
    flush()
    markdown = "\n\n".join(chunks)
    # Standardized rows are document-level. The aggregate Markdown is carried
    # in one page entry, as in the existing SEC benchmark conversion.
    output = {
        "task_type": "parse", "example_id": document_id, "pipeline_name": pipeline_name,
        "pages": [{"page_index": 0, "markdown": markdown}], "markdown": markdown,
    }

    # Required native ParseBench envelope. Timestamps describe conversion,
    # not model inference; latency is not measured here.
    now = datetime.now(timezone.utc).isoformat()
    return {
        "request": {"example_id": document_id, "source_file_path": str(source_file_path),
                    "product_type": "parse"},
        "pipeline_name": pipeline_name, "product_type": "parse",
        "raw_output": {}, "output": output,
        "started_at": now, "completed_at": now, "latency_in_ms": 0,
    }
