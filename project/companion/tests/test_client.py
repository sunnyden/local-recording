import asyncio
import json

import pytest

from recorder_companion._espressif.proto import network_config_pb2 as wifi
from recorder_companion.client import RecorderClient
from recorder_companion.errors import DeviceError, ProtocolError, TransportError


class FakeSecurity:
    def __init__(self, username, password):
        self.ready = False
        self.steps = 0
        self.count = 0

    def handshake(self, response=None):
        self.steps += 1
        self.ready = self.steps == 3
        return None if self.ready else b"handshake"

    def encrypt(self, payload):
        self.count += 1
        return b"encrypted:" + payload

    def decrypt(self, payload):
        assert payload.startswith(b"encrypted:")
        return payload.removeprefix(b"encrypted:")


class FakeTransport:
    def __init__(self):
        self.connections = 0
        self.disconnections = 0
        self.requests = []
        self.fail_handshake = 0
        self.fail_command = False
        self.reject_command = None
        self.auth_states = ["pending", "authorized"]
        self.auth_start_state = "pending"
        self.wifi_states = [1, 0]

    async def connect(self):
        self.connections += 1

    async def disconnect(self):
        self.disconnections += 1

    async def exchange(self, endpoint, payload):
        if endpoint == "proto-ver":
            return b'{"prov":{"ver":"v1.1","sec_ver":2,"sec_patch_ver":1}}'
        if endpoint == "prov-session":
            if self.fail_handshake:
                self.fail_handshake -= 1
                raise TransportError("Disconnected")
            return b"server"
        assert payload.startswith(b"encrypted:")
        plaintext = payload.removeprefix(b"encrypted:")
        if endpoint == "prov-config":
            request = wifi.NetworkConfigPayload()
            request.ParseFromString(plaintext)
            if request.msg == wifi.TypeCmdSetWifiConfig:
                response = wifi.NetworkConfigPayload(msg=wifi.TypeRespSetWifiConfig)
                response.resp_set_wifi_config.SetInParent()
                self.ssid = request.cmd_set_wifi_config.ssid
            elif request.msg == wifi.TypeCmdApplyWifiConfig:
                response = wifi.NetworkConfigPayload(msg=wifi.TypeRespApplyWifiConfig)
                response.resp_apply_wifi_config.SetInParent()
            else:
                response = wifi.NetworkConfigPayload(msg=wifi.TypeRespGetWifiStatus)
                response.resp_get_wifi_status.wifi_sta_state = self.wifi_states.pop(0)
            return b"encrypted:" + response.SerializeToString()
        request = json.loads(plaintext)
        self.requests.append(request)
        if self.fail_command:
            raise TransportError("Lost reply")
        response = {"v": 1, "id": request["id"], "ok": True}
        if request["type"] == self.reject_command:
            response.update(ok=False, code="auth_unavailable", message="not printed")
        elif request["type"] == "auth.start":
            response["state"] = self.auth_start_state
        elif request["type"] == "auth.status":
            response["state"] = self.auth_states.pop(0)
            if response["state"] == "pending":
                response.update(user_code="ABCD-EFGH",
                                verification_uri="https://microsoft.com/devicelogin",
                                expires_in=900)
        return b"encrypted:" + json.dumps(response).encode()


def test_device_rejection_keeps_authenticated_session_usable():
    async def scenario():
        transport = FakeTransport()
        client = RecorderClient(transport, security_factory=FakeSecurity)
        await client.connect("user", "secret")
        security = client.security
        disconnected = transport.disconnections
        transport.reject_command = "auth.start"
        with pytest.raises(DeviceError, match="Check Wi-Fi"):
            await client.request("auth.start", resource="graph")
        assert client.security is security
        assert transport.disconnections == disconnected
        assert (await client.request("status"))["ok"] is True
        await client.close()
    asyncio.run(scenario())


def test_retry_creates_fresh_security_and_never_replays_commands():
    async def scenario():
        transport = FakeTransport()
        transport.fail_handshake = 1
        instances = []
        def factory(*args):
            instance = FakeSecurity(*args)
            instances.append(instance)
            return instance
        client = RecorderClient(transport, security_factory=factory)
        await client.connect("user", "secret")
        assert transport.connections == 2
        assert len(instances) == 2
        transport.fail_command = True
        with pytest.raises(TransportError):
            await client.request("auth.start", resource="graph")
        assert len(transport.requests) == 1
        assert client.security is None
        transport.fail_command = False
        await client.connect("user", "secret")
        await client.request("status")
        assert len(instances) == 3
        assert [r["type"] for r in transport.requests] == ["auth.start", "status"]
    asyncio.run(scenario())


def test_downgrade_stops_before_setup_credentials_or_application_data():
    async def scenario():
        transport = FakeTransport()
        endpoints = []
        async def exchange(endpoint, payload):
            endpoints.append(endpoint)
            return b'{"prov":{"sec_ver":1,"sec_patch_ver":1}}'
        transport.exchange = exchange
        def forbidden_factory(*args):
            pytest.fail("Security credentials must not be used with a downgraded server")
        client = RecorderClient(transport, security_factory=forbidden_factory)
        with pytest.raises(ProtocolError, match="no fallback"):
            await client.connect("user", "secret")
        assert endpoints == ["proto-ver"]
        assert transport.connections == 1
        assert client.security is None
        with pytest.raises(TransportError):
            await client.request("auth.start", resource="graph")
        assert endpoints == ["proto-ver"]
    asyncio.run(scenario())


def test_handshake_cleanup_failure_stops_retry_and_preserves_primary():
    async def scenario():
        transport = FakeTransport()
        transport.fail_handshake = 1
        async def failed_cleanup():
            if transport.connections:
                raise TransportError("BLE disconnect failed.")
        transport.disconnect = failed_cleanup
        client = RecorderClient(transport, security_factory=FakeSecurity)
        with pytest.raises(TransportError, match="Disconnected") as error:
            await client.connect("user", "password")
        assert transport.connections == 1
        assert error.value.cleanup_failed
        assert client.security is None
    asyncio.run(scenario())


def test_wifi_decode_failure_preserved_when_disconnect_fails():
    async def scenario():
        transport = FakeTransport()
        client = RecorderClient(transport, security_factory=FakeSecurity)
        await client.connect("user", "password")
        async def malformed(*args):
            return b"\xff"
        async def failed_cleanup():
            raise TransportError("BLE disconnect failed.")
        client._secure_exchange = malformed
        transport.disconnect = failed_cleanup
        with pytest.raises(ProtocolError, match="failed validation") as error:
            await client.wifi_status()
        assert error.value.cleanup_failed
        assert client.security is None
    asyncio.run(scenario())


def test_wifi_remains_open_for_delayed_device_code_and_finish():
    async def scenario():
        transport = FakeTransport()
        client = RecorderClient(transport, security_factory=FakeSecurity)
        await client.connect("user", "secret")
        assert await client.configure_wifi("\u7f51\u7edc", "wifi-password", poll_interval=0.001) == "connected"
        assert transport.ssid == "\u7f51\u7edc".encode()
        assert client.security is not None
        codes = []
        async def display(uri, code):
            codes.append((uri, code))
        assert await client.authorize("graph", display, poll_interval=0.001) == "authorized"
        assert codes == [("https://microsoft.com/devicelogin", "ABCD-EFGH")]
        assert len({r["id"] for r in transport.requests}) == len(transport.requests)
        await client.request("setup.finish")
        assert client.security is None
    asyncio.run(scenario())


@pytest.mark.parametrize("state", ["denied", "expired", "error", "idle"])
def test_auth_terminal_states(state):
    async def scenario():
        transport = FakeTransport()
        transport.auth_states = [state]
        client = RecorderClient(transport, security_factory=FakeSecurity)
        await client.connect("user", "secret")
        async def display(*args):
            pytest.fail("No code was returned")
        assert await client.authorize("proxy", display, poll_interval=0.001) == state
    asyncio.run(scenario())


def test_existing_authorization_does_not_start_new_grant_or_poll():
    async def scenario():
        transport = FakeTransport()
        transport.auth_start_state = "authorized"
        client = RecorderClient(transport, security_factory=FakeSecurity)
        await client.connect("USERA1B2C3D4", "A" * 24)
        async def display(*args):
            pytest.fail("Already authorized; no browser code is needed")
        assert await client.authorize("proxy", display) == "authorized"
        assert [request["type"] for request in transport.requests] == ["auth.start"]
    asyncio.run(scenario())


def test_auth_cancel_on_local_timeout():
    async def scenario():
        transport = FakeTransport()
        client = RecorderClient(transport, security_factory=FakeSecurity)
        await client.connect("user", "secret")
        async def display(*args):
            pass
        with pytest.raises(Exception, match="expired"):
            await client.authorize("graph", display, timeout=0.001, poll_interval=0.01)
        assert transport.requests[-1]["type"] == "auth.cancel"
    asyncio.run(scenario())


@pytest.mark.parametrize("ssid,password", [
    ("", "password"), ("x" * 33, "password"), ("valid", "short"), ("valid", "x" * 64),
    ("nul\x00ssid", "password"),
])
def test_invalid_wifi_never_reaches_ble(ssid, password):
    async def scenario():
        transport = FakeTransport()
        client = RecorderClient(transport, security_factory=FakeSecurity)
        with pytest.raises(ProtocolError):
            await client.configure_wifi(ssid, password)
        assert transport.connections == 0
    asyncio.run(scenario())
