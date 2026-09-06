import asyncio
import importlib.util
import json
from pathlib import Path
import time
from types import SimpleNamespace

from cryptography.hazmat.primitives.asymmetric import rsa
import httpx
import jwt
import pytest

path = Path(__file__).resolve().parents[1] / "scripts" / "enroll_user.py"
spec = importlib.util.spec_from_file_location("operator_enrollment", path)
enrollment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(enrollment)

from recorder_proxy.auth import AuthError, TokenValidator
from recorder_proxy.config import DISCOVERY, ISSUER, CONSUMER_TENANT

A = "11111111-1111-1111-1111-111111111111"
B = "22222222-2222-2222-2222-222222222222"
USER = "33333333-3333-3333-3333-333333333333"
IDENTITY = {"deviceClientId": A, "apiClientId": B}


def setup_flow(monkeypatch, *, bad_signature=False, audience=B):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    signing_key = rsa.generate_private_key(public_exponent=65537, key_size=2048) if bad_signature else key
    now = int(time.time())
    token = jwt.encode(
        {"aud": audience, "iss": ISSUER, "tid": CONSUMER_TENANT, "oid": USER,
         "azp": A, "scp": "access_as_user", "ver": "2.0",
         "iat": now, "nbf": now, "exp": now + 60},
        signing_key, algorithm="RS256", headers={"kid": "enrollment-test"},
    )
    jwk = json.loads(jwt.algorithms.RSAAlgorithm.to_jwk(key.public_key()))
    jwk.update({"kid": "enrollment-test", "use": "sig", "alg": "RS256"})
    keys_url = "https://login.microsoftonline.com/common/discovery/v2.0/keys"

    def metadata(request):
        if str(request.url) == DISCOVERY:
            return httpx.Response(200, json={"issuer": ISSUER, "jwks_uri": keys_url})
        assert str(request.url) == keys_url
        return httpx.Response(200, json={"keys": [jwk]})

    requests = []

    class OAuthClient:
        async def __aenter__(self):
            return self

        async def __aexit__(self, *args):
            pass

        async def post(self, url, data):
            requests.append((url, data))
            if url.endswith("/devicecode"):
                body = {"user_code": "TEST-CODE", "device_code": "not-printed",
                        "verification_uri": "https://www.microsoft.com/link",
                        "expires_in": 900, "interval": 1}
            else:
                body = {"access_token": token}
            return httpx.Response(200, json=body, request=httpx.Request("POST", url))

    monkeypatch.setattr(enrollment, "httpx", SimpleNamespace(AsyncClient=lambda **kwargs: OAuthClient()))
    monkeypatch.setattr(
        enrollment, "TokenValidator",
        lambda settings: TokenValidator(settings, http=httpx.AsyncClient(transport=httpx.MockTransport(metadata))),
    )
    return token, requests


def test_enrollment_returns_only_fully_validated_identity(monkeypatch, capsys):
    token, requests = setup_flow(monkeypatch)
    result = asyncio.run(enrollment.enroll(IDENTITY))
    captured = capsys.readouterr()
    assert result["oid"] == USER
    assert result["api_audience"] == B
    assert token not in captured.out + captured.err
    assert "not-printed" not in captured.out + captured.err
    assert "TEST-CODE" in captured.err
    assert "https://www.microsoft.com/link" in captured.err
    assert requests[0][1]["scope"] == f"api://{B}/access_as_user"
    assert "offline_access" not in requests[0][1]["scope"]


@pytest.mark.parametrize("bad_signature,audience", [(True, B), (False, A)])
def test_unverified_claims_never_become_enrollment(monkeypatch, capsys, bad_signature, audience):
    token, _ = setup_flow(monkeypatch, bad_signature=bad_signature, audience=audience)
    with pytest.raises(AuthError):
        asyncio.run(enrollment.enroll(IDENTITY))
    captured = capsys.readouterr()
    assert token not in captured.out + captured.err
    assert USER not in captured.out + captured.err


@pytest.mark.parametrize("uri", [
    "http://www.microsoft.com/link",
    "https://www.microsoft.com.evil.example/link",
    "https://evil.example@www.microsoft.com/link",
    "https://www.microsoft.com/link?next=https://evil.example",
])
def test_enrollment_rejects_untrusted_verification_urls(uri):
    with pytest.raises(ValueError):
        enrollment.verification_uri(uri)
