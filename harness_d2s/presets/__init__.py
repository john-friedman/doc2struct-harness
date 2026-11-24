"""Load document transcription presets."""
import json
import re
from importlib.resources import files


def load(preset="harness1"):
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", preset):
        raise ValueError(f"Unknown preset: {preset}")
    from ..tools.grid import submit_grid
    from ..tools.edits import apply_edits
    from ..tools.compact_edits import apply_edits as compact_edits
    resource = files(__package__).joinpath(preset + ".json")
    if not resource.is_file():
        raise ValueError(f"Unknown preset: {preset}")
    config = json.loads(resource.read_text(encoding="utf-8"))
    directory = files(__package__)
    for key in ("prompt", "judge_prompt"):
        filename = config.pop(key + "_file", None)
        if filename:
            config[key] = directory.joinpath(filename).read_text(encoding="utf-8")
    available = {"submit_grid":submit_grid, "apply_edits":apply_edits, "compact_edits":compact_edits}
    config["tools"] = [available[name] for name in config["tools"]]
    return config


__all__ = ["load"]
