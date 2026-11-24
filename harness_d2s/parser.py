"""Transcribe PDFs using document presets and model profiles."""
import asyncio
import base64
from contextlib import AsyncExitStack
import json
import threading

import doc2struct

from .context import render
from .document import Document
from .openrouter import RateLimiter, summarize_usage
from .gateway import create_client, create_async_client, validate_gateway
from .presets import load
from .models import load as load_model

_PARSE_LOCK = threading.Lock()


def parse(document, *, preset="harness1", model="luna", effort=None, gateway="vercel", api_key=None, timeout=180,
          requests_per_second=None, on_complete=None, client=None, **options):
    """Return standardized rows using Vercel (default) or OpenRouter REST APIs."""
    validate_gateway(gateway)
    if load(preset).get('pipeline') in {'judge', 'judge_edits'}:
        async def run():
            async def transport(payload):
                return await asyncio.to_thread(client, payload)
            return await parse_async(document, preset=preset, model=model, effort=effort, gateway=gateway, api_key=api_key,
                                     timeout=timeout, requests_per_second=requests_per_second,
                                     on_complete=on_complete, client=transport if client else None, **options)
        return asyncio.run(run())
    client = client or create_client(gateway, api_key=api_key, timeout=timeout,
                                     rate_limiter=RateLimiter(requests_per_second))
    state, selected, payload = _prepare(document, effort=effort, **(load_model(model) | load(preset) | options))
    response = client(payload)
    try:
        return _apply(state, selected, response)
    finally:
        if on_complete:
            on_complete(summarize_usage([response]))


async def parse_async(document, *, preset="harness1", model="luna", effort=None, gateway="vercel", api_key=None, timeout=180,
                      requests_per_second=None, on_complete=None, client=None, **options):
    """Parse asynchronously; only native PDF extraction runs in a thread."""
    validate_gateway(gateway)
    async with AsyncExitStack() as scope:
        if client is None:
            client = await scope.enter_async_context(create_async_client(
                gateway, api_key=api_key, timeout=timeout, requests_per_second=requests_per_second))
        config = load_model(model) | load(preset) | options
        if config.get('pipeline') in {'judge', 'judge_edits'}:
            if config['pipeline'] == 'judge_edits':
                from .judge_edits import parse_async as judge_parse
            else:
                from .judge import parse_async as judge_parse
            extras = {key: config.pop(key) for key in ('judge_client', 'on_event', 'draft_rows') if key in config}
            if extras.get('judge_client') is None:
                extras['judge_client'] = await scope.enter_async_context(create_async_client(
                    gateway, decisions=True, api_key=api_key, timeout=timeout,
                    requests_per_second=requests_per_second))
            if effort is not None:
                config['reasoning'] = dict(config.get('reasoning') or {}, effort=effort)
            return await judge_parse(document, config=config, client=client, api_key=api_key,
                                     timeout=timeout, on_complete=on_complete, **extras)
        state, selected, payload = await asyncio.to_thread(_prepare, document, effort=effort, **config)
        response = await client(payload)
        try:
            return _apply(state, selected, response)
        finally:
            if on_complete:
                on_complete(summarize_usage([response]))


async def parse_many(documents, *, preset="harness1", model="luna", effort=None, requests_per_second=None,
                     gateway="vercel", api_key=None, timeout=180, client=None, **options):
    """Yield completed (document ID, rows) pairs through one shared client."""
    validate_gateway(gateway)
    async with AsyncExitStack() as scope:
        if client is None:
            client = await scope.enter_async_context(create_async_client(
                gateway, api_key=api_key, timeout=timeout, requests_per_second=requests_per_second))
        if (load(preset) | options).get('pipeline') in {'judge', 'judge_edits'} and options.get('judge_client') is None:
            options['judge_client'] = await scope.enter_async_context(create_async_client(
                gateway, decisions=True, api_key=api_key, timeout=timeout,
                requests_per_second=requests_per_second))

        async def process(ident, document):
            return ident, await parse_async(document, preset=preset, model=model, effort=effort, gateway=gateway,
                                            api_key=api_key, timeout=timeout, requests_per_second=requests_per_second,
                                            client=client, **options)

        pending = {asyncio.create_task(process(ident, document)) for ident, document in documents.items()}
        done = set()
        try:
            while pending:
                done, pending = await asyncio.wait(pending, return_when=asyncio.FIRST_COMPLETED)
                while done:
                    yield done.pop().result()
        finally:
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, *done, return_exceptions=True)


def _prepare(document, *, model, prompt, tools, max_output_tokens=8192, reasoning=None, effort=None,
             provider=None, prompt_cache_options=None, context_format="markdown", rules=None,
             adapter="pdf_adapter_experimental", service_tier=None, document_input="pdf", dpi=144):
    content, format = document
    if effort is not None:
        reasoning = dict(reasoning or {}, effort=effort)
    if format.lower() != "pdf":
        raise ValueError("Only PDF documents are supported")
    if document_input not in {"pdf", "images"}:
        raise ValueError("document_input must be 'pdf' or 'images'")
    selected = {tool.name: tool for tool in tools}
    if set(selected) not in ({"submit_grid"}, {"apply_edits"}):
        raise ValueError("Select submit_grid or apply_edits")
    with _PARSE_LOCK:
        rows = doc2struct.parse_pdf(content, rules=rules, adapter=adapter)
    state = Document(rows)
    inputs = [] if context_format == 'none' else [{"type":"text", "text":render(rows, context_format)}]
    if document_input == "images":
        from .judge import render_pages, image_part
        if dpi <= 0:
            raise ValueError("dpi must be positive")
        with _PARSE_LOCK:
            pages, _ = render_pages(content, dpi)
        for number, page in enumerate(pages, 1):
            inputs.extend([{"type": "text", "text": f"Original document, page {number}:"}, image_part(page)])
    else:
        inputs.append({"type":"file", "file":{"filename":"document.pdf",
            "file_data":"data:application/pdf;base64," + base64.b64encode(content).decode("ascii")}})
    messages = [{"role":"system", "content":prompt}, {"role":"user", "content":inputs}]
    payload = {"model": model, "messages": messages, "stream": False,
               "max_tokens": max_output_tokens,
               "plugins": [{"id": "file-parser", "pdf": {"engine": "native"}}]}
    if selected:
        payload.update(tools=[tool.schema for tool in selected.values()],
                       tool_choice="required", parallel_tool_calls=False)
    payload.update({k: v for k, v in {"reasoning": reasoning, "provider": provider,
                   "prompt_cache_options": prompt_cache_options, "service_tier": service_tier}.items()
                   if v is not None})
    return state, selected, payload


def _apply(state, selected, response, on_complete=None):
    if on_complete:
        on_complete(summarize_usage([response]))
    choice = response["choices"][0]
    if choice.get("finish_reason") == "length" or choice.get('native_finish_reason') in {'max_output_tokens','length'}:
        raise RuntimeError("Model repair plan exceeded max_output_tokens")
    calls = choice["message"].get("tool_calls") or []
    if len(calls) != 1:
        raise RuntimeError("Transcription requires exactly one complete tool call")
    function = calls[0]["function"]
    if function["name"] not in selected:
        raise ValueError(f"Expected {next(iter(selected))}")
    selected[function["name"]](state, **json.loads(function["arguments"]))
    state.finish("Applied single-response repair plan")
    return state.rows


def preset_parse(document=None, *, documents=None, preset="harness1", gateway="vercel", rps=None, **options):
    """Parse one PDF, or return {document_id: rows} for a batch of PDFs."""
    if rps is not None:
        options['requests_per_second'] = rps
    if documents is None:
        return parse(document, preset=preset, gateway=gateway, **options)
    if document is not None:
        raise ValueError('Pass document or documents, not both')

    async def collect():
        return {ident: rows async for ident, rows in parse_many(
            dict(documents), preset=preset, gateway=gateway, **options)}

    return asyncio.run(collect())


preset_parse_async = parse_async
preset_parse_many = parse_many
