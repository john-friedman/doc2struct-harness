"""Non-streaming OpenRouter clients: urllib for sync, aiohttp for async."""
import asyncio
import json
import aiohttp
import os
import time
import threading
import math
from urllib.error import HTTPError
from urllib.request import Request, urlopen

_URL = "https://openrouter.ai/api/v1/chat/completions"


class GatewayHTTPError(RuntimeError):
    """Retain response metadata for retry pacing and billing diagnostics."""
    def __init__(self, name, status, body, headers):
        super().__init__(f"{name} HTTP {status}: {body[:1500]}")
        self.status, self.body = status, body
        self.headers = {k.lower(): v for k, v in headers.items()}


class RateLimiter:
    """Shared API start pacing; network requests overlap after leaving wait()."""
    def __init__(self, requests_per_second=None):
        if requests_per_second is not None and (not math.isfinite(requests_per_second) or requests_per_second <= 0):
            raise ValueError("requests_per_second must be positive and finite")
        self.interval = 1 / requests_per_second if requests_per_second else 0
        self.lock, self.next_start = threading.Lock(), 0.0

    def wait(self):
        with self.lock:
            time.sleep(max(0, self.next_start-time.monotonic()))
            self.next_start = time.monotonic()+self.interval


class OpenRouterClient:
    name = "OpenRouter"
    key_env = "OPENROUTER_API_KEY"
    url = _URL

    def __init__(self, api_key=None, timeout=180, *, rate_limiter=None, endpoint=None, headers=None):
        self.api_key = api_key or os.environ.get(self.key_env)
        if not self.api_key:
            raise ValueError(f"Set {self.key_env} or pass api_key")
        self.timeout = timeout
        self.endpoint = endpoint or self.url
        self.headers = headers or {}
        self.rate_limiter = rate_limiter or RateLimiter()

    def __call__(self, payload):
        self.rate_limiter.wait()
        request = Request(self.endpoint,
                          data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
                          headers={"Authorization": "Bearer " + self.api_key,
                                   "Content-Type": "application/json", "X-Title": "doc2struct-harness", **self.headers})
        # No automatic retry: a timed-out model request may still be billed.
        started = time.perf_counter()
        try:
            with urlopen(request, timeout=self.timeout) as response:
                result = json.load(response)
        except HTTPError as exc:
            body = exc.read().decode("utf-8", errors="replace")
            raise GatewayHTTPError(self.name, exc.code, body, exc.headers) from None
        if result.get("error"):
            raise RuntimeError(self.name + ": " + json.dumps(result["error"]))
        result["_latency_seconds"] = time.perf_counter() - started
        return result


class AsyncRateLimiter:
    """Pace request starts while allowing all in-flight requests to overlap."""
    def __init__(self, requests_per_second=None):
        if requests_per_second is not None and (not math.isfinite(requests_per_second) or requests_per_second <= 0):
            raise ValueError("requests_per_second must be positive and finite")
        self.interval = 1 / requests_per_second if requests_per_second else 0
        self.lock, self.next_start = asyncio.Lock(), 0.0

    async def wait(self):
        async with self.lock:
            await asyncio.sleep(max(0, self.next_start - time.monotonic()))
            self.next_start = time.monotonic() + self.interval


class AsyncOpenRouterClient:
    name = "OpenRouter"
    key_env = "OPENROUTER_API_KEY"
    url = _URL

    def __init__(self, api_key=None, timeout=180, *, requests_per_second=None, endpoint=None, headers=None,
                 max_connections=32):
        self.api_key = api_key or os.environ.get(self.key_env)
        if not self.api_key:
            raise ValueError(f"Set {self.key_env} or pass api_key")
        self.timeout = timeout
        self.endpoint = endpoint or self.url
        self.headers = headers or {}
        self.max_connections = max_connections
        self.rate_limiter = AsyncRateLimiter(requests_per_second)

    async def __aenter__(self):
        self.session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=self.timeout),
            connector=aiohttp.TCPConnector(limit=self.max_connections),
            headers={"Authorization": "Bearer " + self.api_key, "X-Title": "doc2struct-harness", **self.headers},
        )
        return self

    async def __aexit__(self, *exc):
        await self.session.close()

    async def __call__(self, payload):
        await self.rate_limiter.wait()
        started = time.perf_counter()
        async with self.session.post(self.endpoint, json=payload) as response:
            if response.status >= 400:
                body = await response.text()
                raise GatewayHTTPError(self.name, response.status, body, response.headers)
            result = await response.json()
        if result.get("error"):
            raise RuntimeError(self.name + ": " + json.dumps(result["error"]))
        result["_latency_seconds"] = time.perf_counter() - started
        return result


def summarize_usage(responses):
    """Missing usage stays null; reasoning is a subset of completion tokens."""
    def total(getter):
        values = [getter(r.get("usage") or {}) for r in responses]
        return sum(values) if values and all(v is not None for v in values) else None
    result = {
        "input_tokens": total(lambda u: u.get("prompt_tokens")),
        "output_tokens": total(lambda u: u.get("completion_tokens")),
        "cached_input_tokens": total(lambda u: (u.get("prompt_tokens_details") or {}).get("cached_tokens")),
        "cache_write_tokens": total(lambda u: (u.get("prompt_tokens_details") or {}).get("cache_write_tokens")),
        "reasoning_tokens": total(lambda u: (u.get("completion_tokens_details") or {}).get("reasoning_tokens")),
        "cost_usd": total(lambda u: u.get("cost")),
        "requests": len(responses),
    }
    result["uncached_input_tokens"] = (result["input_tokens"] - result["cached_input_tokens"]
        if result["input_tokens"] is not None and result["cached_input_tokens"] is not None else None)
    return result
