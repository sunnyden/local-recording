import json

import httpx


class IntelligenceError(Exception):
    """Stable, non-sensitive public errors; never retain provider response bodies."""

    def __init__(self, code, status=503, retryable=False):
        super().__init__(code)
        self.code, self.status, self.retryable = code, status, retryable


def unavailable():
    return IntelligenceError("temporarily_unavailable", 503, True)


async def bounded_body(response, limit):
    body = bytearray()
    async for chunk in response.aiter_bytes():
        if len(body) + len(chunk) > limit:
            raise unavailable()
        body.extend(chunk)
    return bytes(body)


async def json_body(response, limit=262144):
    try:
        value = json.loads(await bounded_body(response, limit))
    except (ValueError, RecursionError):
        raise unavailable() from None
    if not isinstance(value, dict):
        raise unavailable()
    return value


def check_response(response):
    status = response.status_code
    if 200 <= status < 300:
        return
    if status in (401, 403):
        raise IntelligenceError("consent_required", 403)
    if status == 404:
        raise IntelligenceError("source_not_found", 404)
    if status in (409, 412):
        raise IntelligenceError("output_conflict", 409)
    if status == 429:
        raise IntelligenceError("busy", 429, True)
    raise unavailable()


def http_client():
    return httpx.AsyncClient(timeout=httpx.Timeout(30, connect=5),
                             follow_redirects=False, trust_env=False)
