"""Judge draft blocks, then repair selected sections with independent image calls."""
import asyncio
import base64
from contextlib import AsyncExitStack
from copy import deepcopy
import hashlib
import io
import json
import math

from PIL import Image
import pypdfium2 as pdfium

from .context import render
from .document import Document, validate
from .models import load as load_model
from .openrouter import AsyncOpenRouterClient
from .tools.grid import submit_grid

DECISIONS_URL = "https://openrouter.ai/api/alpha/decisions"


def image_part(image):
    data = io.BytesIO()
    image.save(data, format="PNG")
    return {"type": "image_url", "image_url": {"url": "data:image/png;base64," + base64.b64encode(data.getvalue()).decode("ascii")}}


def intervals(length, limit=1568, overlap=192):
    count = max(1, math.ceil((length-overlap)/(limit-overlap)))
    size = math.ceil((length+(count-1)*overlap)/count)
    return [(start, min(length, start+size)) for start in (i*(size-overlap) for i in range(count))]


def render_pages(content, dpi):
    pages, tiles = [], []
    with pdfium.PdfDocument(content) as pdf:
        for page_index in range(len(pdf)):
            page = pdf[page_index]
            bitmap = page.render(scale=dpi/72)
            image = bitmap.to_pil().convert("RGB").copy()
            bitmap.close()
            page.close()
            pages.append(image)
            for top, bottom in intervals(image.height):
                for left, right in intervals(image.width):
                    tile = image.crop((left, top, right, bottom))
                    # White padding satisfies JEV's minimum dimensions without rescaling.
                    canvas = Image.new("RGB", (max(280, tile.width), max(280, tile.height)), "white")
                    canvas.paste(tile, (0, 0))
                    tiles.append(dict(id=f"p{page_index+1}t{len(tiles)}", page=page_index,
                                      rect=[left, top, right, bottom], image=canvas))
    return pages, tiles


def judge_payload(rows, tiles, config):
    questions = {}
    for row in rows:
        ident = f"b{row['id']}"
        questions[ident+"_fix"] = {"type": "noul", "instructions": f"Does draft block {ident} need fixing against its original source across all tiles, under the review rules?"}
        questions[ident+"_absent"] = {"type": "noul", "instructions": f"Is draft block {ident} invented content with no corresponding source content anywhere in the supplied page tiles?"}
        for tile in tiles:
            key = ident+"_"+tile["id"]
            questions[key+"_present"] = {"type": "noul", "instructions": f"Is original source content corresponding to draft block {ident} visible in tile {tile['id']}?"}
            questions[key+"_box"] = {"type": "box", "instructions": f"All original source content corresponding to draft block {ident} that is visible in tile {tile['id']}, including associated omissions from the draft."}
    for tile in tiles:
        questions[tile["id"]+"_missing"] = {"type": "noul", "instructions": f"Does tile {tile['id']} contain document text completely absent from every draft block, under the review rules?"}
        questions[tile["id"]+"_missing_box"] = {"type": "box", "instructions": f"The document text in tile {tile['id']} completely absent from every draft block; enclose all such omitted text."}
    state = [config["judge_prompt"], "Draft:\n"+render(rows, "edit")]
    for tile in tiles:
        state.extend([f"Tile {tile['id']}; page {tile['page']+1}; source pixel bounds {tile['rect']}.", image_part(tile["image"])])
    return dict(load_model(config["judge_model"]), state=state, questions=questions)


def probability(answers, key):
    answer = answers[key]
    value = answer.get("noul")
    if answer.get("type") != "noul" or not isinstance(value, (float, int)) or not 0 <= value <= 1:
        raise ValueError(f"Invalid judge probability: {key}")
    return value


def source_box(answers, key, tile, pages):
    answer = answers[key]
    box = answer.get("box", {})
    if answer.get("type") != "box" or any(type(box.get(k)) not in (int, float) or not 0 <= box[k] <= 1 for k in ("left", "top", "right", "bottom")):
        raise ValueError(f"Invalid judge box: {key}")
    if box["left"] >= box["right"] or box["top"] >= box["bottom"]:
        raise ValueError(f"Empty judge box: {key}")
    x, y = tile["rect"][:2]
    w, h = tile["image"].size
    page = pages[tile["page"]]
    rect = [max(0, min(page.width, math.floor(x+box['left']*w))),
            max(0, min(page.height, math.floor(y+box['top']*h))),
            max(0, min(page.width, math.ceil(x+box['right']*w))),
            max(0, min(page.height, math.ceil(y+box['bottom']*h)))]
    if rect[0] >= rect[2] or rect[1] >= rect[3]:
        raise ValueError(f"Box outside source page: {key}")
    return rect


def union(a, b):
    return [min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3])]


def overlap(a, b):
    intersection = max(0, min(a[2], b[2])-max(a[0], b[0])) * max(0, min(a[3], b[3])-max(a[1], b[1]))
    return intersection/max(1, min((a[2]-a[0])*(a[3]-a[1]), (b[2]-b[0])*(b[3]-b[1])))


def add_box(boxes, page, rect):
    boxes[page] = union(boxes[page], rect) if page in boxes else list(rect)


def route(rows, pages, tiles, response, threshold, issues=None):
    answers = response["answers"]
    locations, tasks, deleted = {}, [], []
    issues = issues if issues is not None else []

    def located(key, tile):
        try:
            return source_box(answers, key, tile, pages)
        except ValueError as exc:
            issues.append(str(exc))
            return None

    for row in rows:
        ident = row["id"]
        locations[ident] = {}
        for tile in tiles:
            key = f"b{ident}_{tile['id']}"
            if probability(answers, key+"_present") > threshold:
                rect = located(key+"_box", tile)
                if rect:
                    add_box(locations[ident], tile['page'], rect)
        if probability(answers, f"b{ident}_fix") > threshold:
            if locations[ident]:
                tasks.append(dict(ids=[ident], boxes=deepcopy(locations[ident]), missing=False))
            elif probability(answers, f"b{ident}_absent") > threshold:
                deleted.append(ident)
    for tile in tiles:
        if probability(answers, tile["id"]+"_missing") > threshold:
            rect = located(tile['id']+"_missing_box", tile)
            if rect:
                tasks.append(dict(ids=[], boxes={tile['page']: rect}, missing=True))

    positions = {row['id']: i for i, row in enumerate(rows)}
    # Omitted text can share a region with existing draft content. Include those
    # blocks in the replacement, rather than inserting duplicate source text.
    changed = True
    while changed:
        changed = False
        for task in tasks:
            ids = set(task['ids'])
            if task['missing']:
                for ident, boxes in locations.items():
                    if ident not in deleted and any(p in task['boxes'] and overlap(rect, task['boxes'][p]) >= .5 for p, rect in boxes.items()):
                        ids.add(ident)
            if ids:
                start, end = min(positions[i] for i in ids), max(positions[i] for i in ids)
                ids.update(row['id'] for row in rows[start:end+1] if row['id'] not in deleted)
            ordered = sorted(ids, key=positions.get)
            if ordered != task['ids']:
                task['ids'] = ordered
                changed = True
            for ident in ordered:
                for page, rect in locations[ident].items():
                    old = task['boxes'].get(page)
                    add_box(task['boxes'], page, rect)
                    changed |= old != task['boxes'][page]
        merged = []
        for task in tasks:
            match = next((t for t in merged if set(t['ids']) & set(task['ids']) or any(p in t['boxes'] and overlap(rect, t['boxes'][p]) >= .5 for p, rect in task['boxes'].items())), None)
            if match is None:
                merged.append(task)
            else:
                match['ids'] = sorted(set(match['ids']+task['ids']), key=positions.get)
                match['missing'] |= task['missing']
                for page, rect in task['boxes'].items():
                    add_box(match['boxes'], page, rect)
                changed = True
        tasks = merged
    for task in tasks:
        if task['ids']:
            task['position'] = min(positions[i] for i in task['ids'])
        else:
            page = min(task['boxes'])
            rect = task['boxes'][page]
            # Use same-column source neighbors to anchor a wholly missing region.
            following = [(b[1], positions[i]) for i, boxes in locations.items() for p, b in boxes.items()
                         if p == page and b[1] >= rect[3] and min(b[2], rect[2]) > max(b[0], rect[0])]
            prior = [(b[3], positions[i]+1) for i, boxes in locations.items() for p, b in boxes.items()
                     if p == page and b[3] <= rect[1] and min(b[2], rect[2]) > max(b[0], rect[0])]
            task['position'] = min(following)[1] if following else max(prior)[1] if prior else len(rows)
    tasks.sort(key=lambda t: (t['position'], min(t['boxes']), t['boxes'][min(t['boxes'])][1], t['boxes'][min(t['boxes'])][0]))
    return tasks, deleted, locations


def repair_payload(task, rows, pages, config):
    selected = [r for r in rows if r['id'] in task['ids']]
    content = [{"type": "text", "text": "Task: "+("replacement" if selected else "missing-content")+
                f"\nIncludes completely omitted content: {task['missing']}\nAssigned draft:\n"+render(selected, "edit")}]
    for page, rect in sorted(task['boxes'].items()):
        content += [{"type": "text", "text": f"Original section, page {page+1}:"}, image_part(pages[page].crop(rect))]
    tools = [tool.schema for tool in config['tools']]
    prefix = hashlib.sha256((config['prompt']+json.dumps(tools, sort_keys=True)).encode()).hexdigest()[:24]
    payload = {k: config[k] for k in ('model', 'provider', 'reasoning', 'service_tier', 'prompt_cache_options') if k in config}
    payload.update(messages=[{"role": "system", "content": [{"type": "text", "text": config['prompt'], "prompt_cache_breakpoint": {"mode": "explicit"}}]},
                             {"role": "user", "content": content}], tools=tools, tool_choice="required", parallel_tool_calls=False,
                   max_tokens=config['max_output_tokens'], session_id="harness-3-"+prefix, prompt_cache_key="harness-3-"+prefix)
    return payload


def combined_usage(judge_responses, repair_responses):
    def collect(responses, judge=False):
        usages = [r.get('usage') or {} for r in responses]
        return dict(requests=len(usages), input_tokens=sum(u.get('input_tokens' if judge else 'prompt_tokens', 0) for u in usages),
                    output_tokens=sum(u.get('output_tokens' if judge else 'completion_tokens', 0) for u in usages),
                    cached_input_tokens=sum((u.get('prompt_tokens_details') or {}).get('cached_tokens', 0) for u in usages),
                    cache_write_tokens=sum((u.get('prompt_tokens_details') or {}).get('cache_write_tokens', 0) for u in usages),
                    cost_usd=sum(u['cost'] for u in usages) if all(u.get('cost') is not None for u in usages) else None)
    judge, repair = collect(judge_responses, True), collect(repair_responses)
    return dict(judge=judge, repair=repair, cost_usd=judge['cost_usd']+repair['cost_usd'] if judge['cost_usd'] is not None and repair['cost_usd'] is not None else None)


async def parse_async(document, *, config, client, api_key=None, timeout=180, on_complete=None,
                      judge_client=None, on_event=None, draft_rows=None):
    from .parser import _PARSE_LOCK
    import doc2struct
    content, format = document
    if format.lower() != 'pdf':
        raise ValueError('Only PDF documents are supported')
    if not 0 <= config['judge_threshold'] <= 1 or config['dpi'] <= 0:
        raise ValueError('Invalid judge threshold or DPI')
    if [t.name for t in config['tools']] != ['submit_grid']:
        raise ValueError('Judge repairs use the harness1 submit_grid tool')

    def prepare():
        with _PARSE_LOCK:
            rows = deepcopy(draft_rows) if draft_rows is not None else doc2struct.parse_pdf(content, rules=config.get('rules'), adapter=config.get('adapter', 'pdf_adapter_experimental'))
            pages, tiles = render_pages(content, config['dpi'])
        return rows, pages, tiles

    rows, pages, tiles = await asyncio.to_thread(prepare)
    repairs, judge_responses, errors = [], [], []
    async with AsyncExitStack() as scope:
        if judge_client is None:
            judge_client = await scope.enter_async_context(AsyncOpenRouterClient(api_key, timeout, endpoint=DECISIONS_URL))
        response = await judge_client(judge_payload(rows, tiles, config))
        judge_responses.append(response)
        if on_event:
            on_event('judge', response)
        judge_issues = []
        tasks, deleted, locations = route(rows, pages, tiles, response, config['judge_threshold'], judge_issues)
        if on_event:
            on_event('routing', dict(tasks=tasks, deleted_ids=deleted, locations=locations, issues=judge_issues,
                                    pages=[list(p.size) for p in pages], tiles=[{k:v for k,v in t.items() if k != 'image'} for t in tiles]))
        semaphore = asyncio.Semaphore(config.get('repair_concurrency', 4))

        async def repair(index, task):
            async with semaphore:
                result = await client(repair_payload(task, rows, pages, config))
                repairs.append(result)
                if on_event:
                    on_event(f'repair_{index}', result)
                try:
                    from .parser import _apply
                    local = Document([])
                    replacement = _apply(local, {'submit_grid': submit_grid}, result)
                    return task, replacement
                except (ValueError, TypeError, KeyError, RuntimeError) as exc:
                    errors.append(dict(task=index, error=f'{type(exc).__name__}: {exc}'))
                    return task, None

        outputs = []
        # The first real repair warms the shared prompt/tools prefix. No extra
        # warm-up request is billed; remaining small repairs run concurrently.
        if tasks:
            outputs.append(await repair(0, tasks[0]))
            outputs.extend(await asyncio.gather(*(repair(i, t) for i,t in enumerate(tasks[1:], 1))))
    replacements, insertions, removed = {}, {}, set(deleted)
    for task, replacement in outputs:
        if replacement is None:
            continue  # Invalid plans retain the original blocks and remain scored.
        if task['ids']:
            replacements[task['position']] = replacement
            removed.update(task['ids'])
        else:
            insertions.setdefault(task['position'], []).extend(replacement)
    result = []
    for i in range(len(rows)+1):
        result.extend(insertions.get(i, []))
        result.extend(replacements.get(i, []))
        if i < len(rows) and rows[i]['id'] not in removed:
            result.append(deepcopy(rows[i]))
    for i, row in enumerate(result):
        row['id'] = i
    validate(result)
    usage = combined_usage(judge_responses, repairs)
    usage.update(tasks=len(tasks), invalid_repairs=errors, judge_issues=judge_issues, deleted_ids=deleted, retained_blocks=sum(r['id'] not in removed for r in rows), dpi=config['dpi'], judge_threshold=config['judge_threshold'])
    if on_complete:
        on_complete(usage)
    return result
