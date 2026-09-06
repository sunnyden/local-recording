import json
import time

from cryptography.hazmat.primitives.asymmetric import rsa
import httpx
import jwt
import pytest

from recorder_proxy.auth import AuthError, AuthUnavailable, TokenValidator
from recorder_proxy.config import CONSUMER_TENANT, DISCOVERY, ISSUER, JWKS_URLS, Settings


@pytest.fixture(scope="module")
def key():
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


@pytest.fixture
def claims(settings):
    now = int(time.time())
    return {"iss": ISSUER, "aud": settings.api_audience, "tid": CONSUMER_TENANT,
            "oid": settings.allowed_oid, "azp": settings.client_id, "scp": "access_as_user",
            "ver": "2.0", "iat": now - 10, "nbf": now - 10, "exp": now + 60}


def validator(settings, key, kid="test"):
    jwk = json.loads(jwt.algorithms.RSAAlgorithm.to_jwk(key.public_key()))
    jwk.update(kid=kid, use="sig", alg="RS256")
    requests = []

    def handler(request):
        requests.append(str(request.url))
        if str(request.url) == DISCOVERY:
            return httpx.Response(200, json={"issuer": "https://login.microsoftonline.com/{tenantid}/v2.0",
                                           "jwks_uri": sorted(JWKS_URLS)[0]})
        return httpx.Response(200, json={"keys": [jwk]})

    return TokenValidator(settings, httpx.AsyncClient(transport=httpx.MockTransport(handler))), requests


def token(key, claims, headers=None):
    return "Bearer " + jwt.encode(claims, key, algorithm="RS256", headers=headers or {"kid": "test"})


async def test_valid(settings, key, claims):
    auth, requests = validator(settings, key)
    user = await auth.validate(token(key, claims))
    assert user.oid == settings.allowed_oid
    await auth.validate(token(key, claims))
    assert len(requests) == 2
    await auth.close()


@pytest.mark.parametrize("claim,value", [
    ("aud", "00000003-0000-0000-c000-000000000000"),  # Graph
    ("aud", "22222222-2222-4222-8222-222222222222"),  # ID token
    ("aud", ["11111111-1111-4111-8111-111111111111"]),
    ("iss", "https://attacker.invalid"), ("iss", "https://login.microsoftonline.com/consumers/v2.0"),
    ("tid", "11111111-1111-4111-8111-111111111111"),
    ("scp", "Files.ReadWrite"), ("scp", ["access_as_user"]), ("scp", "access_as_user_extra"),
    ("azp", "wrong-client"), ("oid", "wrong-user"), ("ver", "1.0"),
    ("exp", 0), ("nbf", 9999999999), ("iat", 9999999999), ("idtyp", "app"),
])
async def test_claim_negatives(settings, key, claims, claim, value):
    auth, _ = validator(settings, key)
    claims[claim] = value
    with pytest.raises(AuthError):
        await auth.validate(token(key, claims))
    await auth.close()


@pytest.mark.parametrize("missing", ["exp", "nbf", "iat", "scp", "azp", "oid", "tid", "aud", "iss", "ver"])
async def test_required_claims(settings, key, claims, missing):
    auth, _ = validator(settings, key)
    del claims[missing]
    with pytest.raises(AuthError):
        await auth.validate(token(key, claims))
    await auth.close()


@pytest.mark.parametrize("header", [
    {"kid": "test", "jku": "https://attacker.invalid/keys"},
    {"kid": "test", "x5u": "https://attacker.invalid/cert"},
    {"kid": "test", "jwk": {}}, {"kid": "test", "crit": []}, {"kid": "test", "typ": "invalid"},
])
async def test_header_overrides(settings, key, claims, header):
    auth, requests = validator(settings, key)
    with pytest.raises(AuthError):
        await auth.validate(token(key, claims, header))
    assert requests == []
    await auth.close()


async def test_signature_and_algorithms(settings, key, claims):
    auth, _ = validator(settings, key)
    other = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    for encoded in (
        token(other, claims),
        "Bearer " + jwt.encode(claims, "not-an-rsa-key" * 4, algorithm="HS256", headers={"kid": "test"}),
        "Bearer " + jwt.encode(claims, "", algorithm="none", headers={"kid": "test"}),
    ):
        with pytest.raises(AuthError):
            await auth.validate(encoded)
    await auth.close()


async def test_bounded_unknown_kid_refresh(settings, key, claims):
    auth, requests = validator(settings, key)
    await auth.refresh()
    for _ in range(20):
        with pytest.raises(AuthError):
            await auth.validate(token(key, claims, {"kid": "new"}))
    assert len(requests) == 2
    auth.attempted_at -= 31
    auth.keys = {"old": key.public_key()}
    assert (await auth.validate(token(key, claims))).oid == claims["oid"]
    assert len(requests) == 4
    await auth.close()


async def test_discovery_cannot_redirect_keys(settings, key, claims):
    async def handler(request):
        return httpx.Response(200, json={"issuer": ISSUER, "jwks_uri": "https://attacker.invalid"})
    auth = TokenValidator(settings, httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    with pytest.raises(AuthUnavailable):
        await auth.validate(token(key, claims))
    await auth.close()


async def test_metadata_failure_redacted(settings, key, claims):
    def handler(request):
        raise httpx.ConnectError("provider secret token", request=request)
    auth = TokenValidator(settings, httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    with pytest.raises(AuthUnavailable) as exc:
        await auth.validate(token(key, claims))
    assert "secret" not in str(exc.value)
    await auth.close()


def env():
    return {"API_B_CLIENT_ID": "11111111-1111-4111-8111-111111111111",
            "PUBLIC_CLIENT_A_ID": "22222222-2222-4222-8222-222222222222",
            "ALLOWED_USER_OID": "33333333-3333-4333-8333-333333333333",
            "AZURE_CLIENT_ID": "44444444-4444-4444-8444-444444444444",
            "VOICELIVE_ENDPOINT": "https://test.services.ai.azure.com",
            "VOICELIVE_MODEL": "gpt-realtime-2", "VOICELIVE_PROFILE": "native"}


@pytest.mark.parametrize("name", list(env()))
def test_missing_identity_or_endpoint(name):
    config = env()
    del config[name]
    with pytest.raises(ValueError):
        Settings.from_env(config)


@pytest.mark.parametrize("name,value", [
    ("AUTH_DISABLED", "true"), ("JWT_JWKS_URL", "https://attacker.invalid"),
    ("AZURE_CLIENT_SECRET", "secret"), ("MAX_SESSION_SECONDS", "901"),
    ("VOICELIVE_ENDPOINT", "https://test.services.ai.azure.com.attacker.invalid"),
    ("VOICELIVE_ENDPOINT", "http://test.services.ai.azure.com"),
    ("VOICELIVE_ENDPOINT", "https://test.services.ai.azure.com?secret=1"),
    ("VOICELIVE_ENDPOINT", "https://user:pw@test.services.ai.azure.com"),
    ("VOICELIVE_API_VERSION", "2025-04-01-preview"), ("VOICELIVE_PROFILE", "openai-ga"),
])
def test_unsafe_config(name, value):
    config = env()
    config[name] = value
    with pytest.raises(ValueError):
        Settings.from_env(config)


def test_valid_config():
    assert Settings.from_env(env()).model == "gpt-realtime-2"
