"""Typed boundary around Espressif's maintained Security 2 implementation."""

import json

from cryptography.exceptions import InvalidTag
from google.protobuf.message import DecodeError

from ._espressif import proto
from ._espressif.security.security2 import Security2
from .errors import ProtocolError


def validate_security_version(payload):
    try:
        version = json.loads(payload)
        provisioning = version["prov"]
        if type(provisioning["sec_ver"]) is not int or provisioning["sec_ver"] != 2:
            raise ValueError
        if type(provisioning["sec_patch_ver"]) is not int or provisioning["sec_patch_ver"] != 1:
            raise ValueError
    except (ValueError, TypeError, KeyError, UnicodeError):
        raise ProtocolError("Recorder must advertise Security 2 patch 1; no fallback is allowed.") from None


class EspressifSecurity2:
    def __init__(self, username, password):
        # Espressif's host implementation encodes credentials as latin-1.
        # Firmware-generated setup credentials are printable ASCII.
        if any(not isinstance(value, str) or not 1 <= len(value) <= 64
               or not value.isascii() or not value.isprintable()
               for value in (username, password)):
            raise ProtocolError("Use the printable setup credentials displayed on the recorder.")
        self._security = Security2(1, username, password, False)
        self._step = 0
        self.ready = False

    def handshake(self, response=None):
        try:
            if self._step not in (0, 1, 2):
                raise ValueError
            if self._step:
                message = proto.session_pb2.SessionData()
                message.ParseFromString(response)
                expected = ("sr0", "sr1")[self._step - 1]
                expected_message = (proto.sec2_pb2.S2Session_Response0,
                                    proto.sec2_pb2.S2Session_Response1)[self._step - 1]
                if (message.sec_ver != 2 or message.WhichOneof("proto") != "sec2"
                        or message.sec2.msg != expected_message
                        or message.sec2.WhichOneof("payload") != expected
                        or getattr(message.sec2, expected).status != 0):
                    raise ValueError
                if self._step == 1:
                    if (not 1 <= len(message.sec2.sr0.device_pubkey) <= 384
                            or not 1 <= len(message.sec2.sr0.device_salt) <= 64):
                        raise ValueError
                elif (len(message.sec2.sr1.device_nonce) != 12
                      or len(message.sec2.sr1.device_proof) != 64):
                    raise ValueError
            output = self._security.security_session(
                response.decode("latin-1") if response is not None else None
            )
            self._step += 1
            if self._step == 3:
                if output is not None:
                    raise ValueError
                self.ready = True
            return output.encode("latin-1") if output is not None else None
        except (DecodeError, ValueError, TypeError, RuntimeError, OverflowError):
            self.ready = False
            raise ProtocolError("Security 2 authentication failed; check displayed setup credentials.") from None

    def encrypt(self, payload):
        if not self.ready:
            raise ProtocolError("Secure session is not established.")
        try:
            return self._security.encrypt_data(payload)
        except (ValueError, TypeError, RuntimeError, OverflowError):
            self.ready = False
            raise ProtocolError("Secure session encryption failed.") from None

    def decrypt(self, payload):
        if not self.ready:
            raise ProtocolError("Secure session is not established.")
        try:
            return self._security.decrypt_data(payload)
        except (InvalidTag, ValueError, TypeError, RuntimeError, OverflowError):
            self.ready = False
            raise ProtocolError("Secure session authentication failed.") from None
