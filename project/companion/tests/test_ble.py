import asyncio
from types import SimpleNamespace

import pytest
from bleak.exc import BleakError

from recorder_companion.ble import (
    DEFAULT_SERVICE_UUID, USER_DESCRIPTION_UUID, BleTransport, discover,
)
from recorder_companion.errors import ProtocolError, TransportError


class FakeWinRT:
    """Models OS ATT fragmentation; not a claim about untested physical hardware."""

    def __init__(self, *args, **kwargs):
        self.is_connected = False
        self.mtu_size = 23
        self.writes = []
        self.fragments = []
        self.reply = b"reply" * 70
        self.names = {}
        characteristics = []
        # Independent fixture from network_provisioning 1.2.4 manager.c plus
        # the recorder endpoint; do not derive expected names from client code.
        endpoints = {"prov-ctrl", "prov-scan", "prov-session", "prov-config",
                     "proto-ver", "recorder-control"}
        for index, name in enumerate(sorted(endpoints)):
            self.names[index] = name.encode()
            characteristics.append(SimpleNamespace(
                uuid=str(index), properties=["read", "write"],
                descriptors=[SimpleNamespace(uuid=USER_DESCRIPTION_UUID, handle=index)],
            ))
        self.service = SimpleNamespace(characteristics=characteristics)
        self.services = SimpleNamespace(
            get_service=lambda uuid: self.service
            if uuid == "1775244d-6b43-439b-877c-060f2d9bed07" else None
        )

    async def connect(self):
        self.is_connected = True

    async def pair(self):
        pytest.fail("Firmware does not require an OS pairing/bonding prerequisite")

    async def disconnect(self):
        self.is_connected = False

    async def read_gatt_descriptor(self, handle):
        return self.names[handle]

    async def write_gatt_char(self, characteristic, payload, *, response):
        assert response is True
        self.writes.append(bytes(payload))
        # Prepare Write overhead is five ATT bytes. The OS recombines these.
        self.fragments = [payload[i:i + self.mtu_size - 5]
                          for i in range(0, len(payload), self.mtu_size - 5)]

    async def read_gatt_char(self, characteristic, *, use_cached):
        assert use_cached is False
        return self.reply


def test_discovery_matches_recorded_real_board_advertisement(monkeypatch):
    from recorder_companion import ble
    actual_uuid = "1775244d-6b43-439b-877c-060f2d9bed07"

    async def scan(**kwargs):
        assert kwargs["service_uuids"] == [actual_uuid]
        return {
            "board": (
                SimpleNamespace(name="RECORDER_TEST", address="AA:BB:CC:DD:EE:FF"),
                SimpleNamespace(local_name="RECORDER_TEST", service_uuids=[actual_uuid.upper()]),
            )
        }

    monkeypatch.setattr(ble.BleakScanner, "discover", scan)
    assert DEFAULT_SERVICE_UUID == actual_uuid
    found = asyncio.run(discover())
    assert len(found) == 1
    assert found[0].name == "RECORDER_TEST"


def test_default_mtu_uses_one_logical_acknowledged_write():
    async def scenario():
        backend = FakeWinRT()
        transport = BleTransport("address", client_factory=lambda *a, **k: backend)
        await transport.connect()
        payload = b"x" * 406  # Real SRP3072 initial frame with recorder username.
        assert await transport.exchange("prov-session", payload) == backend.reply
        assert backend.writes == [payload]
        assert len(backend.fragments) > 20
        assert b"".join(backend.fragments) == payload
        await transport.disconnect()
    asyncio.run(scenario())


@pytest.mark.parametrize("issue", ["missing", "duplicate", "write-no-response"])
def test_discovery_rejects_invalid_endpoints(issue):
    async def scenario():
        backend = FakeWinRT()
        if issue == "missing":
            backend.service.characteristics.pop()
        elif issue == "duplicate":
            backend.service.characteristics.append(backend.service.characteristics[0])
        else:
            backend.service.characteristics[0].properties = ["read", "write-without-response"]
        transport = BleTransport("address", client_factory=lambda *a, **k: backend)
        with pytest.raises(ProtocolError):
            await transport.connect()
        assert not backend.is_connected
    asyncio.run(scenario())


def test_bounds_and_failed_write_close_connection_without_replay():
    async def scenario():
        backend = FakeWinRT()
        transport = BleTransport("address", client_factory=lambda *a, **k: backend)
        await transport.connect()
        with pytest.raises(ProtocolError):
            await transport.exchange("prov-session", b"x" * 513)
        assert not backend.writes
        backend.reply = b"r" * 513
        with pytest.raises(ProtocolError):
            await transport.exchange("prov-session", b"x")
        assert not backend.is_connected
        assert len(backend.writes) == 1
        with pytest.raises(TransportError):
            await transport.exchange("prov-session", b"x")
    asyncio.run(scenario())


def test_transport_exception_never_echoes_driver_payload(capsys):
    async def scenario():
        backend = FakeWinRT()
        async def fail(*args, **kwargs):
            raise BleakError("password=hidden-wifi access_token=secret")
        backend.write_gatt_char = fail
        transport = BleTransport("address", client_factory=lambda *a, **k: backend)
        await transport.connect()
        with pytest.raises(TransportError) as error:
            await transport.exchange("prov-session", b"secret")
        assert "secret" not in str(error.value)
        assert not backend.is_connected
    asyncio.run(scenario())
    assert "secret" not in capsys.readouterr().out


def test_disconnect_failure_is_explicit_and_redacted():
    async def scenario():
        backend = FakeWinRT()
        async def fail():
            raise OSError("driver secret")
        backend.disconnect = fail
        transport = BleTransport("address", client_factory=lambda *a, **k: backend)
        await transport.connect()
        with pytest.raises(TransportError, match="disconnect failed") as error:
            await transport.disconnect()
        assert "secret" not in str(error.value)
        assert transport.client is None
        assert transport.endpoints == {}
    asyncio.run(scenario())


@pytest.mark.parametrize("primary_type", [BleakError, asyncio.CancelledError, RuntimeError])
@pytest.mark.parametrize("cleanup_type", [BleakError, RuntimeError])
def test_cleanup_failure_preserves_primary_and_cancellation(primary_type, cleanup_type):
    async def scenario():
        backend = FakeWinRT()
        primary = primary_type("primary driver secret")
        async def write_failure(*args, **kwargs):
            raise primary
        async def cleanup_failure():
            raise cleanup_type("secondary driver secret")
        backend.write_gatt_char = write_failure
        backend.disconnect = cleanup_failure
        transport = BleTransport("address", client_factory=lambda *a, **k: backend)
        await transport.connect()
        expected = TransportError if primary_type is BleakError else primary_type
        with pytest.raises(expected) as caught:
            await transport.exchange("prov-session", b"request")
        if primary_type is not BleakError:
            assert caught.value is primary
        assert caught.value.cleanup_failed
        assert caught.value.cleanup_error is not None
        assert "cleanup also failed" in caught.value.__notes__[0]
        assert "secret" not in caught.value.__notes__[0]
        assert transport.client is None
    asyncio.run(scenario())


def test_cancellation_during_disconnect_is_not_swallowed():
    async def scenario():
        backend = FakeWinRT()
        async def cancelled():
            raise asyncio.CancelledError
        backend.disconnect = cancelled
        transport = BleTransport("address", client_factory=lambda *a, **k: backend)
        await transport.connect()
        with pytest.raises(asyncio.CancelledError):
            await transport.disconnect()
        assert transport.client is None
    asyncio.run(scenario())


def test_discovery_does_not_disguise_programming_errors(monkeypatch):
    from recorder_companion import ble

    async def failure(**kwargs):
        raise RuntimeError("unexpected backend bug")
    monkeypatch.setattr(ble.BleakScanner, "discover", failure)
    with pytest.raises(RuntimeError, match="unexpected backend bug"):
        asyncio.run(ble.discover())
