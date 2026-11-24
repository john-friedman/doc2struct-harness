# doc2struct harness

doc2struct is pretty good, and very cheap to run. LLMs are expensive. This is a harness to get the accuracy of LLMs, at a cheaper cost. [Article](https://datamule.xyz/articles/opensourcing-a-cheap-document-harness/)

## Benchmarks

**ParseBench-2**

| Model / mode | Samples | Overall | Text | Formatting | Tables | Forms | Median ¢/page | Mean ¢/page |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| harness1: sol-low | 3,214 | 91.28 | 90.47 | 93.00 | 92.68 | 88.98 | 1.9972 | 2.7863 |
| harness 2 edits: sol-low | 3,214 | 90.64 | 89.73 | 92.42 | 91.41 | 88.98 | 1.7066 | 2.1777 |
| harness 4 (judge + edits): sol-low | 3,214 | 88.66 | 87.31 | 91.92 | 88.00 | 87.42 | 1.2480 | 1.5266 |
| harness1: luna-high | 3,214 | 88.44 | 87.40 | 90.60 | 87.41 | 88.36 | 0.1711 | 0.2190 |
| harness 3 (judge): sol-low | 3,214 | 88.00 | 84.93 | 92.58 | 86.16 | 88.32 | 2.1189 | 2.6777 |
| harness1: deepseek-v4.1-flash-1017-low | 2,140 | 87.23 | 85.33 | 91.35 | 85.69 | 86.56 | 0.5390 | 0.8974 |
| harness 2 edits: luna-high | 3,214 | 86.57 | 85.92 | 90.40 | 81.48 | 88.47 | 0.2312 | 0.2688 |
| harness 2 edits: deepseek-v4.1-flash-1017-low | 2,140 | 86.53 | 84.57 | 91.01 | 83.91 | 86.64 | 0.9056 | 1.1442 |
| harness1: luna-low | 3,214 | 85.80 | 86.28 | 87.57 | 82.24 | 87.11 | 0.1042 | 0.1411 |
| harness 4 (judge + edits): luna-high | 3,214 | 85.50 | 82.07 | 90.72 | 81.56 | 87.65 | 0.2689 | 0.3094 |
| harness1: luna-low (PDF only) | 3,214 | 85.26 | 86.52 | 84.27 | 82.87 | 87.38 | 0.0923 | 0.1155 |
| harness 3 (judge): luna-high | 3,214 | 84.47 | 78.28 | 90.79 | 81.29 | 87.52 | 0.2792 | 0.3259 |
| harness 4 (judge + edits): deepseek-v4.1-flash-1017-low | 2,139 | 83.81 | 81.66 | 89.72 | 79.48 | 84.38 | 1.2700 | 1.5079 |
| harness1: glm-5.3-flash-low | 798 | 83.58 | 81.89 | 88.42 | 81.45 | 82.55 | 0.3045 | 0.4643 |
| harness 3 (judge): deepseek-v4.1-flash-1017-low | 2,139 | 83.42 | 78.08 | 90.02 | 79.63 | 85.97 | 0.7582 | 1.1512 |
| harness 2 edits: luna-low | 3,214 | 83.13 | 82.99 | 87.32 | 76.08 | 86.13 | 0.0943 | 0.1192 |
| harness 3 (judge): luna-low | 3,214 | 82.37 | 77.66 | 89.63 | 76.44 | 85.76 | 0.1700 | 0.2008 |
| harness 3 (judge): glm-5.3-flash-low | 798 | 78.69 | 73.13 | 87.02 | 72.53 | 82.09 | 0.3519 | 0.5656 |
| harness 2 edits: glm-5.3-flash-low | 798 | 73.17 | 70.34 | 80.38 | 61.88 | 80.09 | 0.3028 | 0.4526 |
| harness 4 (judge + edits): glm-5.3-flash-low | 798 | 73.16 | 70.70 | 82.69 | 61.94 | 77.30 | 0.3694 | 0.5853 |
| doc2struct | 3,214 | 57.54 | 50.59 | 68.86 | 42.88 | 67.82 | 0.0000 | 0.0000 |

## Misc

- I had a LLM rewrite [ParseBench](https://github.com/run-llama/ParseBench)'s scorer in C to make it much faster.

> Note: everything below this is AI generated. That's because this repository is the equivalent of scratch paper made public.

## Generating Data

Install from the repository:

```sh
python -m pip install -e .
```

Set `AI_GATEWAY_API_KEY` for Vercel AI Gateway (the default), or
`OPENROUTER_API_KEY` for `gateway="openrouter"`. Both use REST APIs.

```python
from pathlib import Path
from harness_d2s import harness

documents = [
    ("case-1", (Path("document-1.pdf").read_bytes(), "pdf")),
    ("case-2", (Path("document-2.pdf").read_bytes(), "pdf")),
]

results = harness.preset_parse(
    documents=documents,
    preset="harness1",
    model="luna",
    effort="low",
    gateway="vercel",
    rps=5,
)
```

`results` maps each document ID to doc2struct standardized rows. For one PDF:

```python
rows = harness.parse(
    (Path("document.pdf").read_bytes(), "pdf"),
    preset="harness1",
    model="sol",
    effort="low",
)
```

Presets control the parsing approach; model and reasoning effort are independent.

| Preset | Approach |
| :--- | :--- |
| `harness1` | Original PDF and draft Markdown; transcribe the complete document with `submit_grid`. |
| `harness-1-just-pdf` | Original PDF without draft Markdown; complete transcription. |
| `harness-2` | Original PDF and draft; reuse source blocks and rows with `apply_edits`. |
| `harness-3` | JEV selects sections and crops at 144 DPI; transcribe selected sections with `submit_grid`. |
| `harness-4` | JEV selects sections and crops at 144 DPI; repair selected sections with compact edits. |

Model profiles live in `harness_d2s/models/`: `luna`, `sol`, `deepseek`, `glm`
and `jev` (the judge). Profiles include provider routing and defaults; their JSON
files specify the exact model versions. You can also pass a gateway model ID.
`effort="low"` or `"high"` overrides the selected model's default.

The command-line example uses the same API:

```sh
python examples/parse.py document.pdf --preset harness1 --model luna --effort high
```

For asynchronous use, await `harness.parse_async(document, ...)`, or iterate
`harness.parse_many({document_id: document, ...}, ...)` to receive
`(document_id, rows)` pairs as they complete. These functions use
`requests_per_second` instead of `rps`.

## Custom prompts

Override a preset's prompt and tools through the same API:

```python
from harness_d2s.tools import submit_grid

rows = harness.parse(
    (Path("document.pdf").read_bytes(), "pdf"),
    model="luna",
    effort="low",
    prompt=(
        "Read the original PDF and transcribe all content in reading order. "
        "Call submit_grid once: use markdown blocks for prose and rectangular "
        "table arrays for data tables. Preserve visible text and formatting."
    ),
    tools=[submit_grid],
)
```

## Scoring with ParseBench

Convert standardized rows into ParseBench result JSON:

```python
import json
from pathlib import Path
from harness_d2s import markdown_converter

convert = markdown_converter("parsebench")
output = Path("runs/results")
output.mkdir(parents=True, exist_ok=True)

for document_id, rows in results.items():
    result = convert(rows, document_id=document_id)
    (output / f"{document_id}.result.json").write_text(
        json.dumps(result, ensure_ascii=False), encoding="utf-8",
    )
```

The optional C scorer is kept separately in `parsebenchc/`. With a C compiler
and CMake installed, build and run it from the repository root:

```sh
python parsebenchc/build.py
python -m parsebenchc score runs/results data scores.json 24
```

The reference directory must contain ParseBench test JSON files. The benchmark
above reports the mean of Text, Formatting, Tables and Forms; Charts and Visual
Grounding are excluded.

## Project layout

```text
harness_d2s/     Python package: parsing, gateways, models, presets and tools
examples/       Command-line parsing example
parsebenchc/    Standalone benchmark scorer
setup.py        Package metadata and dependencies
```

Local datasets, runs and historical experiments live in ignored `data/`, `runs/`
and `archive/` directories. They are not included in the Python distribution.
