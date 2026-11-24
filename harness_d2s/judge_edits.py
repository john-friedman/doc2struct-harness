"""Frozen JEV routing with section-scoped apply_edits repairs."""
import asyncio
import base64
import io
import math
from contextlib import AsyncExitStack
from copy import deepcopy
from PIL import Image

from .document import Document, validate
from .context import block_markdown
from .judge import (DECISIONS_URL, render_pages, judge_payload, route,
                    repair_payload, combined_usage, overlap, add_box, image_part)
from .openrouter import AsyncOpenRouterClient


def adjust_regions(tasks, rows, pages, deleted, locations, config):
    tasks=deepcopy(tasks);positions={r['id']:i for i,r in enumerate(rows)}
    threshold=config.get('merge_overlap')
    if threshold is not None:
        def close_enough(a,b):
            left,right=sorted([a,b],key=lambda t:min(positions[k] for k in t['ids']))
            start=max(positions[k] for k in left['ids'])+1;end=min(positions[k] for k in right['ids'])
            return sum(len(block_markdown(r)) for r in rows[start:end] if r['id'] not in deleted)<=config.get('merge_gap_chars',0)
        while True:
            pair=next(((i,j) for i,a in enumerate(tasks) for j,b in enumerate(tasks[i+1:],i+1)
                if a['ids'] and b['ids']
                and (set(a['ids'])&set(b['ids']) or (close_enough(a,b)
                     and any(p in b['boxes'] and overlap(box,b['boxes'][p])>=threshold for p,box in a['boxes'].items())))),None)
            if pair is None:break
            i,j=pair;a=tasks[i];b=tasks.pop(j)
            a['ids']=sorted(a['ids']+b['ids'],key=positions.get)
            start=min(positions[k] for k in a['ids']);end=max(positions[k] for k in a['ids'])+1
            a['ids']=[r['id'] for r in rows[start:end] if r['id'] not in deleted]
            for k in a['ids']:
                for p,box in locations.get(k,{}).items():add_box(a['boxes'],p,box)
            a['position']=min(a['position'],b['position']);a['missing']|=b['missing']
            for p,box in b['boxes'].items():add_box(a['boxes'],p,box)
    if config.get('safe_insertions'):
        for task in tasks:
            if task['ids']:continue
            p=min(task['boxes']);box=task['boxes'][p];center=(box[1]+box[3])/2
            following=[((b[1]+b[3])/2,positions[k]) for k,bs in locations.items() if k not in deleted
                       for page,b in bs.items() if page==p and (b[1]+b[3])/2>center and min(b[2],box[2])>max(b[0],box[0])]
            prior=[((b[1]+b[3])/2,positions[k]+1) for k,bs in locations.items() if k not in deleted
                   for page,b in bs.items() if page==p and (b[1]+b[3])/2<=center and min(b[2],box[2])>max(b[0],box[0])]
            task['position']=min(following)[1] if following else max(prior)[1] if prior else len(rows)
            for owner in tasks:
                if not owner['ids']:continue
                start=min(positions[k] for k in owner['ids']);end=max(positions[k] for k in owner['ids'])+1
                if start<task['position']<end:
                    b=owner['boxes'].get(p)
                    task['position']=start if b and center<(b[1]+b[3])/2 else end
    fraction=config.get('crop_padding',0)
    minimum=config.get('min_crop_height',0)
    for task in tasks:
        for p,(x1,y1,x2,y2) in list(task['boxes'].items()):
            w,h=pages[p].size;dx=(x2-x1)*fraction
            dy=max((y2-y1)*fraction,(h*minimum-(y2-y1))/2,0)
            task['boxes'][p]=[max(0,math.floor(x1-dx)),max(0,math.floor(y1-dy)),min(w,math.ceil(x2+dx)),min(h,math.ceil(y2+dy))]
    return sorted(tasks,key=lambda t:(t['position'],min(t['boxes']),t['boxes'][min(t['boxes'])][1]))


async def parse_async(document, *, config, client, api_key=None, timeout=180,
                      on_complete=None, judge_client=None, on_event=None, draft_rows=None):
    from .parser import _PARSE_LOCK, _apply
    import doc2struct
    content, format = document
    if format.lower() != 'pdf':
        raise ValueError('Only PDF documents are supported')
    if not 0 <= config['judge_threshold'] <= 1 or config['dpi'] <= 0:
        raise ValueError('Invalid judge threshold or DPI')
    if [t.name for t in config['tools']] != ['apply_edits']:
        raise ValueError('Section edit repairs require apply_edits')

    def prepare():
        with _PARSE_LOCK:
            rows = deepcopy(draft_rows) if draft_rows is not None else doc2struct.parse_pdf(
                content, rules=config.get('rules'), adapter=config.get('adapter', 'pdf_adapter_experimental'))
            pages, tiles = render_pages(content, config['dpi'])
        return rows, pages, tiles

    rows, pages, tiles = await asyncio.to_thread(prepare)
    repairs, errors, issues = [], [], []
    async with AsyncExitStack() as scope:
        if judge_client is None:
            judge_client = await scope.enter_async_context(AsyncOpenRouterClient(api_key, timeout, endpoint=DECISIONS_URL))
        empty=not any(r.get('matrix') or r.get('runs') or (r.get('text') and not (r['id']==0 and r['type']=='title' and r['text']=='introduction')) for r in rows)
        if empty and config.get('empty_draft_direct'):
            response=None;deleted=[r['id'] for r in rows];locations={}
            tasks=[dict(ids=[],boxes={p:[0,0,page.width,page.height]},missing=True,position=0) for p,page in enumerate(pages)]
        else:
            response = await judge_client(judge_payload(rows, tiles, config))
            tasks, deleted, locations = route(rows, pages, tiles, response, config['judge_threshold'], issues)
            tasks=adjust_regions(tasks,rows,pages,deleted,locations,config)
        if on_event:
            on_event('judge', response)
            on_event('routing', dict(tasks=tasks, deleted_ids=deleted, locations=locations, issues=issues,
                     pages=[list(p.size) for p in pages], tiles=[{k:v for k,v in t.items() if k != 'image'} for t in tiles]))
        semaphore = asyncio.Semaphore(config.get('repair_concurrency', 4))

        async def repair(index, task):
            async with semaphore:
                payload = repair_payload(task, rows, pages, config)
                positions = [i for i,r in enumerate(rows) if r['id'] in task['ids']]
                start = min(positions) if positions else task['position']
                end = max(positions)+1 if positions else start
                neighbors = [next((r for r in candidates if r['id'] not in deleted), None)
                             for candidates in (reversed(rows[:start]), iter(rows[end:]))]
                excerpts = [block_markdown(r)[:400] for r in neighbors if r is not None]
                if excerpts:
                    payload['messages'][1]['content'][0]['text'] += '\n\nRead-only neighboring draft excerpts (handled elsewhere; do not emit):\n'+'\n\n'.join(excerpts)
                limit=config.get('empty_max_side',config.get('repair_max_side')) if empty else config.get('repair_max_side')
                if limit:
                    for i,part in enumerate(payload['messages'][1]['content']):
                        if part['type']!='image_url':continue
                        with Image.open(io.BytesIO(base64.b64decode(part['image_url']['url'].split(',',1)[1]))) as im:
                            cols=max((len(r['matrix'][0]) for r in rows if r['id'] in task['ids'] and r['type']=='table'),default=0)
                            scale=min(1,max(limit/max(im.size),config.get('repair_min_scale',0),
                                            config.get('table_min_column_pixels',0)*cols/im.width))
                            side=math.ceil(max(im.size)*scale)
                            im.thumbnail((side,side),Image.Resampling.LANCZOS)
                            payload['messages'][1]['content'][i]=image_part(im)
                result = await client(payload)
                repairs.append(result)
                if on_event:
                    on_event(f'repair_{index}', result)
                try:
                    # Original IDs remain intact; references cannot access other sections.
                    local = Document([r for r in rows if r['id'] in task['ids']])
                    return task, _apply(local, {'apply_edits': config['tools'][0]}, result)
                except (ValueError, TypeError, KeyError, RuntimeError) as exc:
                    errors.append(dict(task=index, error=f'{type(exc).__name__}: {exc}'))
                    return task, None

        outputs = []
        if tasks:
            outputs.append(await repair(0, tasks[0]))
            outputs.extend(await asyncio.gather(*(repair(i,t) for i,t in enumerate(tasks[1:],1))))
    replacements, insertions, removed = {}, {}, set(deleted)
    for task, replacement in outputs:
        if replacement is None:
            continue
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
    usage = combined_usage([response] if response is not None else [], repairs)
    usage.update(tasks=len(tasks), invalid_repairs=errors, judge_issues=issues, deleted_ids=deleted,
                 retained_blocks=sum(r['id'] not in removed for r in rows), dpi=config['dpi'], judge_threshold=config['judge_threshold'])
    if on_complete:
        on_complete(usage)
    return result
