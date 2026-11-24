"""REST gateway selection; presets use one shared request format."""
from copy import deepcopy
import hashlib

from .openrouter import OpenRouterClient, AsyncOpenRouterClient

VERCEL_CHAT_URL = "https://ai-gateway.vercel.sh/v1/chat/completions"
JEV_HEADERS = {"TypeSafe-Danger-Zone-Images": "true", "TypeSafe-Danger-Zone-Long-Context": "true"}
DECISIONS_URLS = {
    "vercel": "https://ai-gateway.vercel.sh/typesafe/v1/systemone",
    "openrouter": "https://openrouter.ai/api/alpha/decisions",
}


def validate_gateway(gateway):
    if gateway not in DECISIONS_URLS:
        raise ValueError("gateway must be 'vercel' or 'openrouter'")


def vercel_payload(payload):
    """Translate routing/cache extensions without changing model instructions."""
    payload = deepcopy(payload)
    if payload.get("model") == "typesafe/jev-1.14":
        payload["model"] = "typesafe-ai/jev-1.14.0"
    if "state" in payload and "questions" in payload:
        # TypeSafe's native image contract uses asset references, not chat parts.
        assets = payload.get("assets") or {}

        def asset_refs(value):
            if isinstance(value, list):
                return [asset_refs(part) for part in value]
            if isinstance(value, dict):
                if value.get("type") == "image_url":
                    url = value["image_url"]["url"]
                    if not url.startswith(("data:image/png;base64,", "data:image/jpeg;base64,")):
                        raise ValueError("JEV images require inline PNG or JPEG data")
                    header, data = url.split(",", 1)
                    ref = "harness-image-" + hashlib.sha256(url.encode()).hexdigest()
                    assets[ref] = {"type": "image", "format": header.split("/")[1].split(";")[0], "data_base64": data}
                    return ref
                return {key: asset_refs(part) for key, part in value.items()}
            return value

        payload["state"] = asset_refs(payload["state"])
        if assets:
            payload["assets"] = assets
    payload.pop("plugins", None)  # Vercel handles native PDF file parts itself.
    routing = payload.setdefault("providerOptions", {}).setdefault("gateway", {})
    provider = payload.pop("provider", None) or {}
    unsupported = set(provider) - {"only", "order", "allow_fallbacks"}
    if unsupported:
        raise ValueError(f"Unsupported Vercel provider options: {sorted(unsupported)}")
    for key in ("only", "order"):
        if key in provider:
            routing[key] = ["typesafe-ai" if p == "typesafe" else p for p in provider[key]]
    if provider.get("allow_fallbacks") is False and not routing.get("only"):
        if not routing.get("order"):
            raise ValueError("Vercel requires provider.only or provider.order to disable provider fallbacks")
        routing["only"] = routing["order"][:1]
    cache = payload.pop("prompt_cache_options", None)
    if cache and cache.get("mode") == "auto":
        routing["caching"] = "auto"
    # Cache lifetimes differ between providers; use Vercel's provider default.
    # OpenAI caches shared prefixes automatically; keep prompt_cache_key intact.
    for message in payload.get("messages", []):
        content = message.get("content")
        for part in content if isinstance(content, list) else []:
            if part.pop("prompt_cache_breakpoint", None):
                part["cache_control"] = {"type": "ephemeral"}
    if not routing:
        payload["providerOptions"].pop("gateway")
    if not payload["providerOptions"]:
        payload.pop("providerOptions")
    return payload


def vercel_response(response):
    """Expose billed USD through the existing usage callbacks; unknown stays null."""
    metadata = (response.get("provider_metadata") or {}).get("gateway") or {}
    usage = response.get("usage") or {}
    response["usage"] = usage
    if usage.get("cost") is None and metadata.get("cost") is not None:
        usage["cost"] = float(metadata["cost"])
    return response


class VercelClient(OpenRouterClient):
    name, key_env, url = "Vercel", "AI_GATEWAY_API_KEY", VERCEL_CHAT_URL

    def __call__(self, payload):
        return vercel_response(super().__call__(vercel_payload(payload)))


class AsyncVercelClient(AsyncOpenRouterClient):
    name, key_env, url = "Vercel", "AI_GATEWAY_API_KEY", VERCEL_CHAT_URL

    async def __call__(self, payload):
        return vercel_response(await super().__call__(vercel_payload(payload)))


def create_client(gateway="vercel", *, decisions=False, **options):
    validate_gateway(gateway)
    if decisions:
        options["endpoint"] = DECISIONS_URLS[gateway]
        if gateway == "vercel":
            options["headers"] = dict(options.get("headers") or {}, **JEV_HEADERS)
    return (VercelClient if gateway == "vercel" else OpenRouterClient)(**options)


def create_async_client(gateway="vercel", *, decisions=False, **options):
    validate_gateway(gateway)
    if decisions:
        options["endpoint"] = DECISIONS_URLS[gateway]
        if gateway == "vercel":
            options["headers"] = dict(options.get("headers") or {}, **JEV_HEADERS)
    return (AsyncVercelClient if gateway == "vercel" else AsyncOpenRouterClient)(**options)
