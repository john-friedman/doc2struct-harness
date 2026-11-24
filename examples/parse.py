"""Install with `python -m pip install -e .`, then set AI_GATEWAY_API_KEY.

Run: python examples/parse.py document.pdf --preset harness1 --effort high
OpenRouter uses --gateway openrouter and OPENROUTER_API_KEY instead.
"""
import argparse
import json
from pathlib import Path

from harness_d2s import harness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pdf", type=Path, nargs="+")
    parser.add_argument("--preset", default="harness1", choices=[
        "harness1", "harness-1-just-pdf", "harness-2", "harness-3", "harness-4",
    ])
    parser.add_argument("--model", default="luna")
    parser.add_argument("--effort", choices=["low", "high"], default="low")
    parser.add_argument("--gateway", choices=["vercel", "openrouter"], default="vercel")
    args = parser.parse_args()
    documents = [(str(path), (path.read_bytes(), "pdf")) for path in args.pdf]
    rows = harness.preset_parse(
        documents=documents, preset=args.preset, model=args.model,
        effort=args.effort, gateway=args.gateway, rps=5,
    )
    print(json.dumps(rows, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
