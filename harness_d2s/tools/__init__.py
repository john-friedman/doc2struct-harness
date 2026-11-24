"""Importable tools for inspecting and repairing standardized documents."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Tool:
    name: str
    description: str
    parameters: dict

    @property
    def schema(self):
        return {"type": "function", "function": {
            "name": self.name, "description": self.description, "parameters": self.parameters,
        }}

    def __call__(self, document, **arguments):
        return document.execute(self.name, arguments)


from .grid import submit_grid
from .edits import apply_edits

__all__ = ["submit_grid", "apply_edits"]
