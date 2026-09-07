import asyncio
from collections import OrderedDict
from dataclasses import dataclass
import hashlib
import time

from azure.core.exceptions import AzureError
from azure.identity.aio import ManagedIdentityCredential
import httpx

from .config import CONSUMER_TENANT
from .intelligence_errors import IntelligenceError, http_client, json_body, unavailable

GRAPH_SCOPE = "https://graph.microsoft.com/.default"
EXCHANGE_SCOPE = "api://AzureADTokenExchange/.default"
AUTHORITY = f"https://login.microsoftonline.com/{CONSUMER_TENANT}/oauth2/v2.0/token"


@dataclass(repr=False)
class UserContext:
    principal: object
    assertion: str


class GraphTokens:
    """Bounded, memory-only OBO cache; fresh MI assertions on every token exchange."""

    def __init__(self, settings, *, http=None, credential=None):
        self.settings = settings
        self.http = http or http_client()
        self.credential = credential or ManagedIdentityCredential(
            client_id=settings.managed_identity_client_id)
        self.cache = OrderedDict()
        self.lock = asyncio.Lock()

    async def token(self, user, *, refresh=False):
        if (user.principal.oid != self.settings.allowed_oid
                or user.principal.expires_at <= time.time()):
            raise IntelligenceError("authentication_required", 401)
        # Include the validated incoming assertion fingerprint: revoked/renewed
        # user authority must not accidentally select a different token's cache.
        key = (user.principal.oid, hashlib.sha256(user.assertion.encode()).digest())
        async with self.lock:
            cached = self.cache.get(key)
            if not refresh and cached and cached[1] > time.time() + 60:
                self.cache.move_to_end(key)
                return cached[0]
            self.cache.pop(key, None)
            try:
                async with asyncio.timeout(15):
                    credential = await self.credential.get_token(EXCHANGE_SCOPE)
                    data = {
                        "client_id": self.settings.api_audience,
                        "client_assertion_type": "urn:ietf:params:oauth:client-assertion-type:jwt-bearer",
                        "client_assertion": credential.token,
                        "grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer",
                        "requested_token_use": "on_behalf_of",
                        "assertion": user.assertion,
                        "scope": GRAPH_SCOPE,
                    }
                    async with self.http.stream("POST", AUTHORITY, data=data) as response:
                        if response.status_code in (400, 401, 403):
                            raise IntelligenceError("consent_required", 403)
                        if response.status_code != 200:
                            raise unavailable()
                        result = await json_body(response, 65536)
            except (AzureError, httpx.HTTPError, TimeoutError):
                raise unavailable() from None
            token, expires = result.get("access_token"), result.get("expires_in")
            if (not isinstance(token, str) or not 1 <= len(token) <= 32768
                    or type(expires) is not int or not 0 < expires <= 86400
                    or result.get("token_type", "").lower() != "bearer"):
                raise unavailable()
            self.cache[key] = (token, min(time.time() + expires, user.principal.expires_at))
            while len(self.cache) > 8:
                self.cache.popitem(last=False)
            return token

    async def close(self):
        self.cache.clear()
        try:
            await self.http.aclose()
        finally:
            await self.credential.close()
