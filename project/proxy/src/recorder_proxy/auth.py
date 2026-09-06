import asyncio
from dataclasses import dataclass
import json
import time

import httpx
import jwt

from .config import CONSUMER_TENANT, DISCOVERY, ISSUER, JWKS_URLS


class AuthError(Exception):
    pass


class AuthUnavailable(Exception):
    pass


@dataclass(frozen=True)
class Principal:
    oid: str
    expires_at: int


class TokenValidator:
    """Only consumer delegated v2 API-B tokens; no token-directed metadata."""

    def __init__(self, settings, http=None):
        self.settings = settings
        self.http = http or httpx.AsyncClient(timeout=5, follow_redirects=False, trust_env=False)
        self.keys = {}
        self.loaded_at = 0.0
        self.attempted_at = float("-inf")
        self.lock = asyncio.Lock()
        self.available = False

    async def _document(self, url):
        try:
            async with self.http.stream("GET", url) as response:
                response.raise_for_status()
                body = bytearray()
                async for chunk in response.aiter_bytes():
                    if len(body) + len(chunk) > 262144:
                        raise AuthUnavailable()
                    body.extend(chunk)
                value = json.loads(body)
        except (httpx.HTTPError, ValueError, RecursionError):
            raise AuthUnavailable() from None
        if not isinstance(value, dict):
            raise AuthUnavailable()
        return value

    async def refresh(self, force=False):
        async with self.lock:
            now = time.monotonic()
            if self.keys and not force and now - self.loaded_at < 3600:
                return
            if now - self.attempted_at < 30:
                if not self.keys or now - self.loaded_at >= 3600:
                    raise AuthUnavailable()
                return
            self.attempted_at = now
            metadata = await self._document(DISCOVERY)
            if metadata.get("issuer") not in (
                ISSUER, "https://login.microsoftonline.com/{tenantid}/v2.0"
            ) or metadata.get("jwks_uri") not in JWKS_URLS:
                raise AuthUnavailable()
            document = await self._document(metadata["jwks_uri"])
            entries = document.get("keys")
            if not isinstance(entries, list) or not 1 <= len(entries) <= 100:
                raise AuthUnavailable()
            keys = {}
            try:
                for item in entries:
                    if not isinstance(item, dict):
                        raise AuthUnavailable()
                    if item.get("kty") != "RSA" or item.get("use", "sig") != "sig":
                        continue
                    if item.get("alg", "RS256") != "RS256":
                        continue
                    issuer = item.get("issuer", ISSUER)
                    if issuer not in (ISSUER, "https://login.microsoftonline.com/{tenantid}/v2.0"):
                        continue
                    kid = item.get("kid")
                    if not isinstance(kid, str) or not kid or kid in keys:
                        raise AuthUnavailable()
                    keys[kid] = jwt.PyJWK.from_dict(item, algorithm="RS256").key
            except (jwt.PyJWTError, ValueError, KeyError, TypeError):
                raise AuthUnavailable() from None
            if not keys:
                raise AuthUnavailable()
            self.keys, self.loaded_at, self.available = keys, now, True

    async def validate(self, authorization):
        if not authorization or not authorization.startswith("Bearer "):
            raise AuthError()
        token = authorization[7:]
        if not token or len(token) > 16384 or any(c.isspace() for c in token):
            raise AuthError()
        try:
            header = jwt.get_unverified_header(token)
        except jwt.PyJWTError:
            raise AuthError() from None
        kid = header.get("kid")
        if (header.get("alg") != "RS256" or not isinstance(kid, str) or not 1 <= len(kid) <= 256
                or any(k in header for k in ("jku", "x5u", "jwk", "crit"))
                or header.get("typ", "JWT") not in ("JWT", "at+jwt")):
            raise AuthError()
        await self.refresh()
        if kid not in self.keys:
            await self.refresh(force=True)
        if kid not in self.keys:
            raise AuthError()
        try:
            claims = jwt.decode(
                token, self.keys[kid], algorithms=["RS256"],
                audience=self.settings.api_audience, issuer=ISSUER, leeway=0,
                options={"require": ["exp", "nbf", "iat", "iss", "aud", "tid", "oid", "azp", "scp", "ver"],
                         "strict_aud": True},
            )
        except jwt.PyJWTError:
            raise AuthError() from None
        if (claims["tid"] != CONSUMER_TENANT or claims["ver"] != "2.0"
                or claims["azp"] != self.settings.client_id
                or claims["oid"] != self.settings.allowed_oid
                or claims.get("idtyp") == "app"
                or not isinstance(claims["scp"], str)
                or "access_as_user" not in claims["scp"].split()
                or any(type(claims[k]) is not int for k in ("exp", "iat", "nbf"))):
            raise AuthError()
        return Principal(claims["oid"], claims["exp"])

    async def close(self):
        self.available = False
        await self.http.aclose()
