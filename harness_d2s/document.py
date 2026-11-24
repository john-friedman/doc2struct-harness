"""Transactional operations on standardized blocks, not generated Python code."""
from copy import deepcopy


def validate(rows):
    ids = set()
    for block in rows:
        ident = block.get("id")
        if type(ident) is not int or ident in ids:
            raise ValueError("Each block needs a unique integer id")
        ids.add(ident)
        if type(block.get("level", 0)) is not int or block.get("level", 0) < 0:
            raise ValueError("level must be a nonnegative integer")
        if not isinstance(block.get("type"), str) or not block["type"]:
            raise ValueError("Block type must be a nonempty string")
        for key in ("text", "preamble", "postamble"):
            if block.get(key) is not None and not isinstance(block[key], str):
                raise ValueError(f"{key} must be a string or null")
        matrix = block.get("matrix") or []
        if not isinstance(matrix, list) or any(not isinstance(r, list) or any(not isinstance(v, str) for v in r) for r in matrix):
            raise ValueError("matrix must be arrays of plain strings")
        if block["type"] == "table" and (not matrix or not matrix[0] or len({len(r) for r in matrix}) != 1):
            raise ValueError("A table must have a nonempty rectangular matrix")
        if block["type"] == "list" and any(len(r) != 2 or not r[0].isdigit() for r in matrix):
            raise ValueError("List rows must be [relative_depth_as_string, item_text]")
        if not isinstance(block.get("runs") or [], list):
            raise ValueError("runs must be a list")
        for run in block.get("runs") or []:
            if not isinstance(run.get("text"), str) or type(run.get("flags", 0)) is not int:
                raise ValueError("Invalid styled text run")
        for cell in block.get("cell_runs") or []:
            r, c = cell.get("row"), cell.get("col")
            if type(r) is not int or type(c) is not int or not (0 <= r < len(matrix) and 0 <= c < len(matrix[r])):
                raise ValueError("cell_runs points outside matrix")
            for run in cell.get("runs") or []:
                if not isinstance(run.get("text"), str) or type(run.get("flags", 0)) is not int:
                    raise ValueError("Invalid styled cell run")
    return {"blocks": len(rows), "tables": sum(b["type"] == "table" for b in rows)}


class Document:
    def __init__(self, rows):
        self.allowed_fields = {"id","level","type","text","matrix","cleaned","preamble","footnotes","postamble","runs","cell_runs"} | {k for b in rows for k in b}
        self.rows = deepcopy(rows)
        self.next_id = max((b["id"] for b in rows), default=-1) + 1
        self.finished = False
        self.note = ""
        self.audit = []

    def block(self, ident):
        found = [b for b in self.rows if b["id"] == ident]
        if len(found) != 1:
            raise ValueError(f"Unknown block id {ident}")
        return found[0]


    def replace_blocks(self, ids, blocks, reason="", after_id=None):
        if len(set(ids)) != len(ids):
            raise ValueError("Duplicate replacement ids")
        for ident in ids:
            self.block(ident)
        if not isinstance(blocks, list):
            raise ValueError("blocks must be a list")
        original = self.rows
        position = min((i for i, b in enumerate(original) if b["id"] in ids), default=len(original))
        if not ids and after_id is not None:
            self.block(after_id)
            position = next(i + 1 for i, b in enumerate(original) if b["id"] == after_id)
        added = []
        for value in blocks:
            value = deepcopy(value)
            unknown = set(value) - self.allowed_fields
            if unknown:
                raise ValueError(f"Non-standard block fields: {sorted(unknown)}")
            value["id"] = self.next_id
            self.next_id += 1
            value.setdefault("level", 0)
            if value.get("runs"):
                for run in value["runs"]:
                    run.setdefault("flags", 0)
                styled_text = "".join(run["text"] for run in value["runs"])
                if not value.get("text"):
                    value["text"] = styled_text
                elif "".join(value["text"].split()) != "".join(styled_text.split()):
                    raise ValueError("runs must cover the complete text")
            added.append(value)
        before = [b for i, b in enumerate(original) if i < position and b["id"] not in ids]
        after = [b for i, b in enumerate(original) if i >= position and b["id"] not in ids]
        self.rows = before + added + after
        return {"new_ids": [b["id"] for b in added], "removed_ids": ids, "reason": reason}


    def finish(self, reason):
        result = validate(self.rows)
        self.finished, self.note = True, reason
        return dict(result, submitted=True, reason=reason)

    def execute(self, name, args):
        if self.finished:
            raise ValueError("Document is already submitted")
        snapshot, next_id = deepcopy(self.rows), self.next_id
        try:
            result = getattr(self, name)(**args)
            validate(self.rows)
        except Exception:
            self.rows, self.next_id = snapshot, next_id
            self.finished = False
            raise
        self.audit.append({"name": name, "arguments": args, "result": result})
        return result


