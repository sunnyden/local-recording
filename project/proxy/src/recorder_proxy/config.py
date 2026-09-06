from dataclasses import dataclass
import os
from urllib.parse import urlsplit
from uuid import UUID


API_VERSION = "2026-07-15"
CONSUMER_TENANT = "9188040d-6c67-4c5b-b112-36a304b66dad"
ISSUER = f"https://login.microsoftonline.com/{CONSUMER_TENANT}/v2.0"
DISCOVERY = "https://login.microsoftonline.com/consumers/v2.0/.well-known/openid-configuration"
JWKS_URLS = {
    "https://login.microsoftonline.com/consumers/discovery/v2.0/keys",
    "https://login.microsoftonline.com/common/discovery/v2.0/keys",
}


def required(env, key):
    value = env.get(key, "").strip()
    if not value:
        raise ValueError(f"{key} must be explicitly configured")
    return value


def guid(value, key):
    try:
        parsed = str(UUID(value))
    except ValueError:
        raise ValueError(f"{key} must be a UUID") from None
    if parsed != value.lower():
        raise ValueError(f"{key} must be a canonical UUID")
    return parsed


@dataclass(frozen=True)
class Settings:
    api_audience: str
    client_id: str
    allowed_oid: str
    managed_identity_client_id: str
    endpoint: str
    model: str
    profile: str
    max_session_seconds: int = 900
    queue_frames: int = 25
    output_queue_frames: int = 15
    max_unplayed_samples: int = 3200
    queue_age_seconds: float = 0.5
    handshake_seconds: float = 15
    clear_timeout_seconds: float = 2
    playback_stall_seconds: float = 2

    @classmethod
    def from_env(cls, env=None):
        env = os.environ if env is None else env
        for name in (
            "AUTH_ISSUER", "AUTH_JWKS_URL", "JWT_ISSUER", "JWT_JWKS_URL",
            "AUTH_DISCOVERY_URL", "JWT_ALGORITHMS", "AUTH_DISABLED",
            "AZURE_CLIENT_SECRET", "VOICELIVE_API_KEY",
        ):
            if name in env:
                raise ValueError(f"{name} is not supported")
        endpoint = required(env, "VOICELIVE_ENDPOINT")
        url = urlsplit(endpoint)
        if (url.scheme != "https" or not url.hostname
                or not url.hostname.endswith((".services.ai.azure.com", ".cognitiveservices.azure.com"))
                or url.username or url.password or url.port not in (None, 443)
                or url.path not in ("", "/") or url.query or url.fragment):
            raise ValueError("VOICELIVE_ENDPOINT must be a trusted Azure resource HTTPS origin")
        if env.get("VOICELIVE_API_VERSION", API_VERSION) != API_VERSION:
            raise ValueError(f"This adapter is pinned to Voice Live {API_VERSION}")
        profile = required(env, "VOICELIVE_PROFILE")
        if profile not in ("native", "byom-azure-openai-realtime"):
            raise ValueError("Unsupported VOICELIVE_PROFILE")
        model = required(env, "VOICELIVE_MODEL")
        if len(model) > 128 or any(not (c.isalnum() or c in "-_.") for c in model):
            raise ValueError("Invalid VOICELIVE_MODEL")
        cap = int(env.get("MAX_SESSION_SECONDS", "900"))
        if not 1 <= cap <= 900:
            raise ValueError("MAX_SESSION_SECONDS must be in 1..900")
        return cls(
            api_audience=guid(required(env, "API_B_CLIENT_ID"), "API_B_CLIENT_ID"),
            client_id=guid(required(env, "PUBLIC_CLIENT_A_ID"), "PUBLIC_CLIENT_A_ID"),
            allowed_oid=guid(required(env, "ALLOWED_USER_OID"), "ALLOWED_USER_OID"),
            managed_identity_client_id=guid(required(env, "AZURE_CLIENT_ID"), "AZURE_CLIENT_ID"),
            endpoint=f"https://{url.hostname}",
            model=model, profile=profile, max_session_seconds=cap,
        )
