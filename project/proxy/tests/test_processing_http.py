import asyncio
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
import hashlib
from pathlib import Path
import time

from cryptography.hazmat.primitives.asymmetric import rsa
import httpx
import pytest
from starlette.requests import ClientDisconnect
from starlette.testclient import TestClient

from recorder_proxy.app import create_app, until_disconnect
from recorder_proxy.config import CONSUMER_TENANT, ISSUER
from recorder_proxy.graph import GraphClient
from recorder_proxy.speech import FastTranscription
from intelligence_fakes import Credential, Drive, SpeechService, Tokens
from test_auth import token, validator
from test_sessions import FakeProvider, HELLO


@pytest.fixture
def api(settings, monkeypatch):
    monkeypatch.chdir(Path(__file__).resolve().parents[1])
    settings = replace(settings, recording_processing_enabled=True,
                       speech_endpoint="https://unit.cognitiveservices.azure.com")
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    claims = {"iss": ISSUER, "aud": settings.api_audience, "tid": CONSUMER_TENANT,
              "oid": settings.allowed_oid, "azp": settings.client_id, "scp": "access_as_user",
              "ver": "2.0", "iat": int(time.time()) - 10, "nbf": int(time.time()) - 10,
              "exp": int(time.time()) + 300}
    auth, _ = validator(settings, key)
    drive, service = Drive(), SpeechService()

    def factory(user):
        graph = GraphClient(settings, Tokens(), user,
                            http=httpx.AsyncClient(transport=httpx.MockTransport(drive.handle)))
        graph.owns_http = True
        return graph

    speech = FastTranscription(settings, credential=Credential(),
                               http=httpx.AsyncClient(transport=httpx.MockTransport(service.handle)))
    app = create_app(settings, validator=auth, provider_factory=lambda _: FakeProvider(),
                     graph_factory=factory, speech=speech)
    body = {"v": 1, "drive_id": "drive", "item_id": "audio",
            "source_sha1": hashlib.sha1(drive.content["audio"]).hexdigest(),
            "source_size": len(drive.content["audio"])}
    with TestClient(app) as client:
        yield client, body, key, claims, drive, service
    assert not list(Path.cwd().glob(".recording-*.wav"))


def test_sync_process_and_status_contract(api):
    client, body, key, claims, drive, service = api
    headers = {"Authorization": token(key, claims)}
    status = client.post("/v1/recordings/status", json=body, headers=headers)
    assert status.status_code == 200 and status.json()["status"] == "not_started"
    response = client.post("/v1/recordings/process", json=body, headers=headers)
    assert response.status_code == 200
    result = response.json()
    assert set(result) == {"v", "operation_id", "status", "json_item_id", "text_item_id"}
    assert result["status"] == "completed" and len(response.content) < 512
    assert client.post("/v1/recordings/status", json=body, headers=headers).json()["status"] == "already_completed"
    assert len(service.calls) == 1


@pytest.mark.parametrize("change", [
    {"aud": "00000003-0000-0000-c000-000000000000"}, {"aud": "client-A"},
    {"azp": "wrong-client"}, {"oid": "wrong-user"}, {"tid": "arbitrary-tenant"},
    {"exp": 1}, {"scp": "Files.ReadWrite"},
])
def test_process_and_status_use_same_b_validator(api, change):
    client, body, key, claims, drive, service = api
    for path in ("/v1/recordings/process", "/v1/recordings/status"):
        response = client.post(path, json=body, headers={"Authorization": token(key, {**claims, **change})})
        assert response.status_code == 401
        assert response.json() == {"v": 1, "error": {"code": "authentication_required", "retryable": False}}
    assert not drive.calls and not service.calls


@pytest.mark.parametrize("payload,media", [
    (b"x" * 4097, "application/json"), (b"{}", "text/plain"), (b"\xff", "application/json"),
    (b'{"v":1,"v":1}', "application/json"), (b"[]", "application/json"),
    (b'{"v":NaN}', "application/json"),
], ids=["oversized", "wrong_media", "non_utf8", "duplicate_keys", "array", "nonfinite"])
def test_request_bounds_and_schema_errors(api, payload, media):
    client, _, key, claims, drive, service = api
    response = client.post("/v1/recordings/process", content=payload,
                           headers={"Authorization": token(key, claims), "Content-Type": media})
    assert response.status_code == 400
    assert response.json()["error"] == {"code": "invalid_request", "retryable": False}
    assert not drive.calls and not service.calls


def test_missing_duplicate_auth_and_query_rejected(api):
    client, body, key, claims, drive, _ = api
    assert client.post("/v1/recordings/process", json=body).status_code == 401
    auth = token(key, claims)
    response = client.post("/v1/recordings/process", json=body,
                           headers=[("Authorization", auth), ("Authorization", auth)])
    assert response.status_code == 401
    response = client.post("/v1/recordings/process?token=untrusted", json=body,
                           headers={"Authorization": auth})
    assert response.status_code == 400 and not drive.calls


def test_processing_does_not_block_voice_and_duplicate_is_not_queued(api):
    client, body, key, claims, drive, service = api
    service.wait = asyncio.Event()
    headers = {"Authorization": token(key, claims)}
    with ThreadPoolExecutor(max_workers=1) as pool:
        future = pool.submit(client.post, "/v1/recordings/process", json=body, headers=headers)
        try:
            deadline = time.monotonic() + 3
            while not service.calls:
                assert time.monotonic() < deadline
                time.sleep(0.005)
            with client.websocket_connect("/v1/voice", headers=headers,
                                           subprotocols=["recorder.voice.v1"]) as socket:
                socket.send_json(HELLO)
                assert socket.receive_json()["state"] == "connecting"
                assert socket.receive_json()["type"] == "ready"
                status = client.post("/v1/recordings/status", json=body, headers=headers)
                assert status.json()["status"] == "processing"
                duplicate = client.post("/v1/recordings/process", json=body, headers=headers)
                assert duplicate.status_code == 409
                assert duplicate.json()["error"]["code"] == "processing_in_progress"
                socket.send_json({"v": 1, "type": "stop"})
        finally:
            client.portal.call(service.wait.set)
        assert future.result(timeout=3).status_code == 200
    assert len(service.calls) == 1


async def test_client_disconnect_cancels_operation():
    started, cleaned = asyncio.Event(), asyncio.Event()

    async def operation():
        started.set()
        try:
            await asyncio.Event().wait()
        finally:
            cleaned.set()

    class Request:
        async def receive(self):
            await started.wait()
            return {"type": "http.disconnect"}

    with pytest.raises(ClientDisconnect):
        await until_disconnect(Request(), operation())
    assert cleaned.is_set()
