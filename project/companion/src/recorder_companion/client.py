import asyncio
import time
import uuid

from google.protobuf.message import DecodeError

from ._espressif.proto import network_config_pb2 as wifi
from .errors import CompanionError, ProtocolError, TransportError, cleanup_preserving_failure
from .protocol import decode_response, encode_request
from .security import EspressifSecurity2, validate_security_version


class RecorderClient:
    def __init__(self, transport, *, security_factory=EspressifSecurity2):
        self.transport = transport
        self.security_factory = security_factory
        self.security = None
        self._lock = asyncio.Lock()

    async def close(self):
        self.security = None
        await self.transport.disconnect()

    async def connect(self, username, password, *, attempts=2):
        if type(attempts) is not int or not 1 <= attempts <= 3:
            raise ProtocolError("Connection attempts must be between one and three.")
        await self.close()
        for attempt in range(attempts):
            try:
                await self.transport.connect()
                validate_security_version(await self.transport.exchange("proto-ver", b"---"))
                security = self.security_factory(username, password)
                request = security.handshake()
                for _ in range(2):
                    response = await self.transport.exchange("prov-session", request)
                    request = security.handshake(response)
                if request is not None or not security.ready:
                    raise ProtocolError("Security 2 handshake did not finish.")
                self.security = security
                return
            except BaseException as error:
                await cleanup_preserving_failure(self.close, error)
                if (not isinstance(error, TransportError) or attempt + 1 == attempts
                        or getattr(error, "cleanup_failed", False)):
                    raise
                await asyncio.sleep(0.5 * (attempt + 1))

    async def _secure_exchange(self, endpoint, plaintext):
        # Serialize encryption, I/O and decryption, not just the BLE write.
        # Security 2 patch 1 shares a monotonically advancing nonce counter.
        async with self._lock:
            if self.security is None:
                raise TransportError("Connect with fresh setup credentials first.")
            try:
                encrypted = self.security.encrypt(plaintext)
                response = await self.transport.exchange(endpoint, encrypted)
                return self.security.decrypt(response)
            except BaseException as error:
                await cleanup_preserving_failure(self.close, error)
                raise

    async def request(self, kind, **fields):
        request_id = uuid.uuid4().hex
        payload = encode_request(kind, request_id, **fields)
        reply = await self._secure_exchange("recorder-control", payload)
        try:
            result = decode_response(reply, request_id, kind)
        except ProtocolError as error:
            await cleanup_preserving_failure(self.close, error)
            raise
        if kind == "setup.finish":
            await self.close()
        return result

    async def _wifi_exchange(self, request, expected_type, response_field):
        reply = await self._secure_exchange("prov-config", request.SerializeToString())
        try:
            response = wifi.NetworkConfigPayload()
            response.ParseFromString(reply)
            if (response.msg != expected_type or not response.HasField(response_field)
                    or getattr(response, response_field).status != 0):
                raise ValueError
            return getattr(response, response_field)
        except (DecodeError, ValueError):
            error = ProtocolError("Wi-Fi provisioning response failed validation.")
            await cleanup_preserving_failure(self.close, error)
            raise error from None

    async def wifi_status(self):
        request = wifi.NetworkConfigPayload(msg=wifi.TypeCmdGetWifiStatus)
        request.cmd_get_wifi_status.SetInParent()
        reply = await self._wifi_exchange(
            request, wifi.TypeRespGetWifiStatus, "resp_get_wifi_status"
        )
        states = {0: "connected", 1: "connecting", 2: "disconnected", 3: "failed"}
        if reply.wifi_sta_state not in states:
            raise ProtocolError("Unknown Wi-Fi connection state.")
        return states[reply.wifi_sta_state]

    async def configure_wifi(self, ssid, password, *, timeout=90.0, poll_interval=2.0):
        try:
            ssid_bytes, password_bytes = ssid.encode("utf-8"), password.encode("utf-8")
            if (not 1 <= len(ssid_bytes) <= 32 or b"\x00" in ssid_bytes
                    or b"\x00" in password_bytes
                    or not (len(password_bytes) == 0 or 8 <= len(password_bytes) <= 63)):
                raise ValueError
        except (ValueError, AttributeError, UnicodeError):
            raise ProtocolError("Wi-Fi SSID must be 1-32 bytes; password empty or 8-63 bytes.") from None
        if not 0 < timeout <= 180 or not 0 < poll_interval <= 10:
            raise ProtocolError("Invalid Wi-Fi timeout.")
        request = wifi.NetworkConfigPayload(msg=wifi.TypeCmdSetWifiConfig)
        request.cmd_set_wifi_config.ssid = ssid_bytes
        request.cmd_set_wifi_config.passphrase = password_bytes
        await self._wifi_exchange(request, wifi.TypeRespSetWifiConfig, "resp_set_wifi_config")
        request = wifi.NetworkConfigPayload(msg=wifi.TypeCmdApplyWifiConfig)
        request.cmd_apply_wifi_config.SetInParent()
        await self._wifi_exchange(request, wifi.TypeRespApplyWifiConfig, "resp_apply_wifi_config")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = await self.wifi_status()
            if state == "connected":
                return state
            if state == "failed":
                raise CompanionError("Wi-Fi connection failed; check credentials on the recorder.")
            await asyncio.sleep(poll_interval)
        raise CompanionError("Wi-Fi connection timed out; check status before configuring again.")

    async def authorize(self, resource, show_code, *, timeout=900.0, poll_interval=2.0):
        if not 0 < timeout <= 1800 or not 0 < poll_interval <= 30:
            raise ProtocolError("Invalid authorization timeout.")
        # The ESP owns all OAuth network calls and polling intervals.
        reply = await self.request("auth.start", resource=resource)
        deadline = time.monotonic() + timeout
        shown = None
        try:
            while time.monotonic() < deadline:
                if "user_code" in reply and reply["user_code"] != shown:
                    deadline = min(deadline, time.monotonic() + reply["expires_in"])
                    shown = reply["user_code"]
                    await show_code(reply["verification_uri"], shown)
                state = reply.get("state", "pending")
                if state in {"authorized", "denied", "expired", "error", "idle"}:
                    return state
                await asyncio.sleep(poll_interval)
                reply = await self.request("auth.status", resource=resource)
            raise CompanionError("Authorization expired; start a new sign-in on the recorder.")
        except BaseException as error:
            if self.security is not None:
                try:
                    await self.request("auth.cancel", resource=resource)
                except CompanionError:
                    error.add_note(
                        "Authorization cancellation was not confirmed; recorder timeout remains active."
                    )
            raise
