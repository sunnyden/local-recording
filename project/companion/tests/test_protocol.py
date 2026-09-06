import json

import pytest

from recorder_companion.errors import DeviceError, ProtocolError
from recorder_companion.protocol import decode_response, encode_request, verification_uri


@pytest.mark.parametrize("uri", [
    "http://microsoft.com/devicelogin",
    "https://microsoft.com.evil.example/devicelogin",
    "https://microsoft.com@evil.example/devicelogin",
    "https://evil.example@microsoft.com/devicelogin",
    "https://microsoft.com:443/devicelogin",
    "https://microsoft.com/devicelogin?redirect=https://evil.example",
    "https://microsoft.com/devicelogin#evil",
    "https://microsoft.com/other",
    "https://www.microsoft.com.evil.example/link",
    "https://evil.example@www.microsoft.com/link",
    "https://www.microsoft.com/link?redirect=https://evil.example",
    "https://www.microsoft.com/link#evil",
    "https://microsoft.com/devicelogin\n",
    "https://microsoft.com\\@evil.example/devicelogin",
    "file:///C:/secret",
    "javascript:alert(1)",
    "https://m\u0456crosoft.com/devicelogin",
    None,
])
def test_rejects_unsafe_browser_url(uri):
    with pytest.raises(ProtocolError):
        verification_uri(uri)


@pytest.mark.parametrize("uri", [
    "https://microsoft.com/devicelogin",
    "https://www.microsoft.com/devicelogin",
    "https://www.microsoft.com/link",
    "https://login.microsoftonline.com/common/oauth2/deviceauth",
])
def test_accepts_explicit_microsoft_browser_urls(uri):
    assert verification_uri(uri) == uri


@pytest.mark.parametrize("kind,fields", [
    ("unknown", {}), ("auth.start", {"resource": "all"}),
    ("auth.status", {"resource": "graph", "password": "secret"}),
    ("auth.unlink", {}), ("auth.unlink", {"confirm": 1}),
    ("status", {"extra": True}),
    ("auth.start", {"resource": []}),
    ("auth.status", {"resource": True}),
    ([], {}),
])
def test_request_validation(kind, fields):
    with pytest.raises(ProtocolError):
        encode_request(kind, "request1", **fields)


def test_valid_request_and_explicit_unlink():
    assert json.loads(encode_request("auth.start", "abc", resource="graph")) == {
        "v": 1, "id": "abc", "type": "auth.start", "resource": "graph",
    }
    assert json.loads(encode_request("auth.unlink", "abc", confirm=True))["confirm"] is True


@pytest.mark.parametrize("value", [
    {"v": True, "id": "abc", "ok": True},
    {"v": 2, "id": "abc", "ok": True},
    {"v": 1, "id": "wrong", "ok": True},
    {"v": 1, "id": "abc", "ok": "true"},
    {"v": 1, "id": "abc", "ok": True, "state": "other"},
    {"v": 1, "id": "abc", "ok": True, "state": "pending", "device_code": "secret"},
    {"v": 1, "id": "abc", "ok": True, "state": "pending",
     "nested": {"refresh_token": "secret"}},
])
def test_response_validation(value):
    with pytest.raises(ProtocolError):
        decode_response(json.dumps(value).encode(), "abc", "auth.status")


@pytest.mark.parametrize("payload", [
    b'{"v":1,"v":1,"id":"abc","ok":true}',
    b'{"v":1,"id":"abc","ok":true,"x":NaN}',
    b"\xff", b"{}", b"x" * 4097,
])
def test_malformed_and_oversized_response(payload):
    with pytest.raises(ProtocolError):
        decode_response(payload, "abc", "status")


def test_remote_error_is_redacted():
    raw = b'{"v":1,"id":"abc","ok":false,"code":"my-password","message":"my-token"}'
    with pytest.raises(DeviceError) as error:
        decode_response(raw, "abc", "status")
    assert "my-password" not in str(error.value)
    assert "my-token" not in str(error.value)


def test_pending_auth_can_include_public_code():
    reply = {"v": 1, "id": "abc", "ok": True, "state": "pending",
             "user_code": "ABCD-EFGH", "verification_uri": "https://microsoft.com/devicelogin",
             "expires_in": 900}
    assert decode_response(json.dumps(reply).encode(), "abc", "auth.status") == reply


def test_pending_auth_accepts_observed_consumer_verification_uri():
    reply = {
        "v": 1, "id": "actual-uri", "ok": True, "state": "pending",
        "user_code": "ABCD-EFGH", "verification_uri": "https://www.microsoft.com/link",
        "expires_in": 899,
    }
    assert decode_response(json.dumps(reply).encode(), "actual-uri", "auth.status") == reply


def test_effective_ble_plaintext_boundary():
    prefix = b'{"v":1,"id":"abc","ok":true,"padding":"'
    suffix = b'"}'
    payload = prefix + b"x" * (496 - len(prefix) - len(suffix)) + suffix
    assert len(payload) == 496
    assert decode_response(payload, "abc", "status")["ok"] is True
    with pytest.raises(ProtocolError):
        decode_response(payload[:-2] + b'x"}', "abc", "status")


@pytest.mark.parametrize("kind,request_id,response", [
    ("auth.start", "start", {"v": 1, "id": "start", "ok": True, "state": "pending"}),
    ("auth.status", "async-code", {
        "v": 1, "id": "async-code", "user_code": "ABCD-EFGH",
        "verification_uri": "https://microsoft.com/devicelogin",
        "expires_in": 899, "ok": True, "state": "pending",
    }),
    ("auth.status", "status", {
        "v": 1, "id": "status", "ok": True, "state": "authorized",
    }),
    ("auth.cancel", "cancel", {"v": 1, "id": "cancel", "ok": True}),
    ("setup.finish", "finish", {"v": 1, "id": "finish", "ok": True}),
    ("status", "status", {
        "v": 1, "id": "status", "ok": True, "wifi": "connected",
        "auth": {"graph": "authorized", "proxy": "pending"},
    }),
    ("auth.unlink", "unlink", {"v": 1, "id": "unlink", "ok": True}),
])
def test_production_firmware_response_contract(kind, request_id, response):
    # Synthetic examples supplied from firmware control.c and its host tests.
    payload = json.dumps(response, separators=(",", ":")).encode("ascii")
    assert len(payload) <= 496
    assert decode_response(payload, request_id, kind) == response


def test_production_firmware_error_is_reported_without_server_message():
    response = {
        "v": 1, "id": "x", "ok": False, "code": "invalid_resource",
        "message": "Request rejected; check device status",
    }
    with pytest.raises(DeviceError, match="Recorder rejected") as error:
        decode_response(json.dumps(response).encode("ascii"), "x", "auth.start")
    assert response["message"] not in str(error.value)
