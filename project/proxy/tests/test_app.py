import time

from cryptography.hazmat.primitives.asymmetric import rsa
import pytest
from starlette.testclient import TestClient, WebSocketDenialResponse

from recorder_proxy.app import create_app
from recorder_proxy.config import CONSUMER_TENANT, ISSUER
from test_auth import token, validator
from test_sessions import FakeProvider, HELLO


@pytest.fixture
def deployment(settings):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    claims = {"iss": ISSUER, "aud": settings.api_audience, "tid": CONSUMER_TENANT,
              "oid": settings.allowed_oid, "azp": settings.client_id, "scp": "access_as_user",
              "ver": "2.0", "iat": int(time.time()) - 10, "nbf": int(time.time()) - 10,
              "exp": int(time.time()) + 60}
    auth, _ = validator(settings, key)
    providers = []

    def factory(config):
        provider = FakeProvider()
        providers.append(provider)
        return provider

    app = create_app(settings, validator=auth, provider_factory=factory)
    with TestClient(app) as client:
        yield client, key, claims, providers
    assert all(p.closed for p in providers)


def connect(client, authorization=None, path="/v1/voice", protocols=None):
    return client.websocket_connect(
        path, headers={"Authorization": authorization} if authorization else {},
        subprotocols=protocols if protocols is not None else ["recorder.voice.v1"],
    )


def test_health_and_authenticated_session(deployment):
    client, key, claims, providers = deployment
    assert client.get("/healthz").status_code == 200
    assert client.get("/readyz").status_code == 200
    with connect(client, token(key, claims)) as socket:
        assert socket.accepted_subprotocol == "recorder.voice.v1"
        socket.send_json(HELLO)
        assert socket.receive_json()["state"] == "connecting"
        assert socket.receive_json()["type"] == "ready"
        assert len(providers) == 1
        assert providers[0].opened.is_set()
        socket.send_json({"v": 1, "type": "stop"})


@pytest.mark.parametrize("problem", ["missing", "graph", "id", "wrong_client", "expired", "query", "subprotocol"])
def test_denied_before_upstream(deployment, problem):
    client, key, claims, providers = deployment
    path, protocols = "/v1/voice", None
    if problem == "graph":
        claims["aud"] = "00000003-0000-0000-c000-000000000000"
    if problem == "id":
        claims["aud"] = claims["azp"]
        del claims["scp"]
    if problem == "wrong_client":
        claims["azp"] = "wrong"
    if problem == "expired":
        claims["exp"] = int(time.time()) - 1
    if problem == "query":
        path += "?token=must-not-be-accepted"
    if problem == "subprotocol":
        protocols = []
    authorization = None if problem == "missing" else token(key, claims)
    with pytest.raises(WebSocketDenialResponse) as exc:
        with connect(client, authorization, path, protocols):
            pytest.fail("Unauthorized upgrade")
    assert exc.value.status_code in (400, 401)
    assert providers == []


def test_single_session_cap_and_release(deployment):
    client, key, claims, providers = deployment
    authorization = token(key, claims)
    with connect(client, authorization) as first:
        first.send_json(HELLO)
        first.receive_json()
        assert first.receive_json()["type"] == "ready"
        with pytest.raises(WebSocketDenialResponse) as exc:
            with connect(client, authorization):
                pytest.fail("Second billable session")
        assert exc.value.status_code == 429
        assert len(providers) == 1
    with connect(client, authorization) as second:
        second.send_json(HELLO)
        second.receive_json()
        assert second.receive_json()["type"] == "ready"
        assert len(providers) == 2


def test_unknown_control_explicit_error(deployment):
    client, key, claims, providers = deployment
    with connect(client, token(key, claims)) as socket:
        socket.send_json(HELLO)
        socket.receive_json()
        socket.receive_json()
        socket.receive_json()
        socket.send_json({"v": 1, "type": "tools.execute", "arguments": "private"})
        assert socket.receive_json() == {"v": 1, "type": "state", "state": "stopping"}
        error = socket.receive_json()
        assert error["code"] == "protocol_error" and "private" not in str(error)
