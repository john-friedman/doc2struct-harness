"""Submit complete document prose and rectangular table grids."""
from . import Tool
from ..markdown import convert_markdown, PARSER, convert_html


class GridTool(Tool):
    def __call__(self, document, blocks):
        rows = []
        for block in blocks:
            if set(block) == {"markdown"}:
                rows.extend(convert_markdown(block["markdown"]))
            elif set(block) == {"table"}:
                matrix = block["table"]
                if not matrix or not any(matrix):
                    raise ValueError("Table grid must be nonempty")
                # Preserve every value in its supplied column.
                width = max(map(len, matrix))
                matrix = [list(row) + [""] * (width - len(row)) for row in matrix]
                values, styles = [], []
                for row_index, row in enumerate(matrix):
                    line = []
                    for col_index, value in enumerate(row):
                        parsed = convert_html("<p>" + PARSER.renderInline(value) + "</p>")
                        parts = [run for paragraph in parsed for run in paragraph.get("runs", [])]
                        line.append("".join(part["text"] for part in parts))
                        if any(part["flags"] for part in parts):
                            styles.append({"row": row_index, "col": col_index, "runs": parts})
                    values.append(line)
                rows.append({"type": "table", "matrix": values, "cell_runs": styles})
            else:
                raise ValueError("Each block must contain exactly markdown or table")
        return document.execute("replace_blocks", {
            "ids": [row["id"] for row in document.rows], "blocks": rows,
        })


submit_grid = GridTool(
    "submit_grid",
    "Submit the complete document in reading order: Markdown prose blocks and rectangular table grids.",
    {
        "type": "object",
        "properties": {
            "blocks": {
                "type": "array",
                "items": {
                    "oneOf": [
                        {
                            "type": "object",
                            "properties": {"markdown": {"type": "string"}},
                            "required": ["markdown"],
                            "additionalProperties": False,
                        },
                        {
                            "type": "object",
                            "properties": {
                                "table": {
                                    "type": "array",
                                    "items": {"type": "array", "items": {"type": "string"}},
                                },
                            },
                            "required": ["table"],
                            "additionalProperties": False,
                        },
                    ],
                },
            },
        },
        "required": ["blocks"],
        "additionalProperties": False,
    },
)

