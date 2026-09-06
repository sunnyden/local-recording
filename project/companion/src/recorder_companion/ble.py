"""Espressif endpoint discovery and acknowledged GATT exchanges using Bleak."""

import asyncio
from dataclasses import dataclass

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from .errors import ProtocolError, TransportError, cleanup_preserving_failure

# Firmware may use a custom 128-bit service UUID; it must be explicitly selected.
DEFAULT_SERVICE_UUID = "1775244d-6b43-439b-877c-060f2d9bed07"
USER_DESCRIPTION_UUID = "00002901-0000-1000-8000-00805f9b34fb"
REQUIRED_ENDPOINTS = frozenset({"prov-session", "prov-config", "proto-ver", "recorder-control"})
MAX_GATT_VALUE = 512
BLE_ERRORS = (BleakError, OSError, TimeoutError)


@dataclass(frozen=True)
class Advertisement:
    device: object
    name: str
    address: str


async def discover(service_uuid=DEFAULT_SERVICE_UUID, timeout=8.0):
    try:
        found = await BleakScanner.discover(
            timeout=timeout, return_adv=True, service_uuids=[service_uuid]
        )
        return [
            Advertisement(device, adv.local_name or device.name or "Recorder", device.address)
            for device, adv in found.values()
            if service_uuid.lower() in {value.lower() for value in adv.service_uuids}
        ]
    except BLE_ERRORS:
        raise TransportError("Bluetooth discovery failed; check Windows Bluetooth.") from None


class BleTransport:
    def __init__(self, device, service_uuid=DEFAULT_SERVICE_UUID, *, timeout=20.0,
                 client_factory=BleakClient):
        self.device = device
        self.service_uuid = service_uuid.lower()
        self.timeout = timeout
        self.client_factory = client_factory
        self.client = None
        self.endpoints = {}
        self._lock = asyncio.Lock()

    async def connect(self):
        await self.disconnect()
        try:
            # WinRT's uncached reads are essential: a cached characteristic value
            # is an old encrypted reply and would desynchronize Security 2 nonces.
            self.client = self.client_factory(
                self.device, timeout=self.timeout, winrt={"use_cached_services": False}
            )
            async with asyncio.timeout(self.timeout):
                await self.client.connect()
                service = self.client.services.get_service(self.service_uuid)
                if service is None:
                    raise ProtocolError("Recorder provisioning service is missing.")
                for characteristic in service.characteristics:
                    for descriptor in characteristic.descriptors:
                        if descriptor.uuid.lower() != USER_DESCRIPTION_UUID:
                            continue
                        value = bytes(await self.client.read_gatt_descriptor(descriptor.handle))
                        if len(value) > 64:
                            raise ProtocolError("Invalid endpoint descriptor.")
                        try:
                            name = value.decode("ascii").rstrip("\x00")
                        except UnicodeDecodeError:
                            raise ProtocolError("Invalid endpoint descriptor.") from None
                        if name not in REQUIRED_ENDPOINTS:
                            continue
                        if name in self.endpoints:
                            raise ProtocolError("Duplicate endpoint descriptor.")
                        if not {"read", "write"} <= set(characteristic.properties):
                            raise ProtocolError("Endpoint lacks acknowledged read/write support.")
                        self.endpoints[name] = characteristic
                if not REQUIRED_ENDPOINTS <= self.endpoints.keys():
                    raise ProtocolError("Required provisioning endpoint is missing.")
        except BLE_ERRORS:
            error = TransportError("BLE connection or endpoint discovery failed.")
            await cleanup_preserving_failure(self.disconnect, error)
            raise error from None
        except BaseException as error:
            await cleanup_preserving_failure(self.disconnect, error)
            raise

    async def disconnect(self):
        client, self.client = self.client, None
        self.endpoints.clear()
        if client is not None:
            try:
                async with asyncio.timeout(self.timeout):
                    await client.disconnect()
            except BLE_ERRORS:
                raise TransportError("BLE disconnect failed; check Windows Bluetooth.") from None

    async def exchange(self, endpoint, payload):
        if not isinstance(payload, bytes) or not 0 < len(payload) <= MAX_GATT_VALUE:
            raise ProtocolError("GATT request exceeds the supported 512-byte value limit.")
        async with self._lock:
            if self.client is None or not self.client.is_connected:
                raise TransportError("BLE disconnected; establish a fresh secure session.")
            if endpoint not in self.endpoints:
                raise ProtocolError("Unknown provisioning endpoint.")
            try:
                async with asyncio.timeout(self.timeout):
                    # One logical protocomm message. WinRT handles ATT long-write
                    # fragmentation (Prepare/Execute) and long reads at any MTU.
                    # Splitting into ordinary writes would dispatch partial messages.
                    await self.client.write_gatt_char(
                        self.endpoints[endpoint], payload, response=True
                    )
                    reply = bytes(await self.client.read_gatt_char(
                        self.endpoints[endpoint], use_cached=False
                    ))
                if not 0 < len(reply) <= MAX_GATT_VALUE:
                    raise ProtocolError("Invalid or oversized GATT response.")
                return reply
            except BLE_ERRORS:
                error = TransportError("BLE exchange failed; reconnect before continuing.")
                await cleanup_preserving_failure(self.disconnect, error)
                raise error from None
            except BaseException as error:
                await cleanup_preserving_failure(self.disconnect, error)
                raise
