import json
import re
from urllib.parse import urlsplit

from .errors import DeviceError, ProtocolError

MAX_JSON_BYTES = 4096
MAX_BLE_PLAINTEXT_BYTES = 496
RESOURCE_TYPES = {"graph", "proxy"}
AUTH_STATES = {"idle", "pending", "authorized", "denied", "expired", "error"}
REQUEST_TYPES = {
    "status", "auth.start", "auth.status", "auth.cancel", "auth.unlink", "setup.finish",
}
FORBIDDEN_FIELDS = {
    "access_token", "refresh_token", "id_token", "device_code", "password",
    "passphrase", "setup_password", "upload_url", "uploadurl", "authorization",
}
VERIFICATION_URLS = {
    ("microsoft.com", "/devicelogin"),
    ("www.microsoft.com", "/devicelogin"),
    ("www.microsoft.com", "/link"),
    ("login.microsoftonline.com", "/common/oauth2/deviceauth"),
}


def verification_uri(value):
    if not isinstance(value, str) or len(value) > 256 or not value.isascii():
        raise ProtocolError("Unsafe Microsoft verification URL.")
    try:
        parsed = urlsplit(value)
        if (parsed.scheme != "https" or parsed.username or parsed.password
                or parsed.port is not None or parsed.query or parsed.fragment
                or (parsed.netloc, parsed.path) not in VERIFICATION_URLS
                or any(ord(c) <= 32 or ord(c) == 127 for c in value)):
            raise ValueError
    except ValueError:
        raise ProtocolError("Unsafe Microsoft verification URL.") from None
    return value


def encode_request(kind, request_id, **fields):
    if (not isinstance(kind, str) or kind not in REQUEST_TYPES or not isinstance(request_id, str)
            or re.fullmatch(r"[\x21-\x7e]{1,64}", request_id) is None):
        raise ProtocolError("Invalid recorder request.")
    if kind in {"auth.start", "auth.status", "auth.cancel"}:
        if (set(fields) != {"resource"} or not isinstance(fields["resource"], str)
                or fields["resource"] not in RESOURCE_TYPES):
            raise ProtocolError("Resource must be graph or proxy.")
    elif kind == "auth.unlink":
        if set(fields) != {"confirm"} or fields["confirm"] is not True:
            raise ProtocolError("Unlink requires explicit confirmation.")
    elif fields:
        raise ProtocolError("Unexpected request fields.")
    encoded = json.dumps({"v": 1, "id": request_id, "type": kind, **fields},
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    if len(encoded) > min(MAX_JSON_BYTES, MAX_BLE_PLAINTEXT_BYTES):
        raise ProtocolError("Recorder request is too large.")
    return encoded


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError
        result[key] = value
    return result


def _reject_constant(_):
    raise ValueError


def _no_secrets(value, depth=0):
    if depth > 8:
        raise ValueError
    if isinstance(value, dict):
        for key, item in value.items():
            if key.lower() in FORBIDDEN_FIELDS:
                raise ValueError
            _no_secrets(item, depth + 1)
    elif isinstance(value, list):
        for item in value:
            _no_secrets(item, depth + 1)


def decode_response(payload, request_id, kind):
    try:
        if (not isinstance(payload, bytes)
                or not 0 < len(payload) <= min(MAX_JSON_BYTES, MAX_BLE_PLAINTEXT_BYTES)):
            raise ValueError
        value = json.loads(payload, object_pairs_hook=_unique_object,
                           parse_constant=_reject_constant)
        if (not isinstance(value, dict) or type(value.get("v")) is not int
                or value["v"] != 1 or value.get("id") != request_id
                or type(value.get("ok")) is not bool):
            raise ValueError
        _no_secrets(value)
        if not value["ok"]:
            if not isinstance(value.get("code"), str) or len(value["code"]) > 64:
                raise ValueError
            # Only local, allowlisted explanations reach the terminal.
            raise DeviceError(value["code"])
        if kind == "auth.status" and value.get("state") not in AUTH_STATES:
            raise ValueError
        if (kind == "auth.start" and "user_code" not in value
                and value.get("state") not in {"pending", "authorized"}):
            raise ValueError
        if any(key in value for key in ("user_code", "verification_uri", "expires_in")):
            if (not isinstance(value.get("user_code"), str)
                    or re.fullmatch(r"[A-Za-z0-9-]{1,64}", value["user_code"]) is None
                    or type(value.get("expires_in")) is not int
                    or not 1 <= value["expires_in"] <= 1800):
                raise ValueError
            verification_uri(value.get("verification_uri"))
        return value
    except (ValueError, TypeError, KeyError, UnicodeError, RecursionError):
        raise ProtocolError("Invalid recorder response.") from None
