"""Bounded managed-identity Fast Transcription probe using synthetic silence."""
import io
import json
import os
import time
import wave
from urllib.parse import urlsplit
from uuid import UUID

import httpx
from azure.core.exceptions import AzureError
from azure.identity import ManagedIdentityCredential


def endpoint_url(endpoint):
    parsed = urlsplit(endpoint)
    if (parsed.scheme != "https" or parsed.username or parsed.password
            or parsed.port not in (None, 443) or parsed.query or parsed.fragment
            or parsed.path not in ("", "/") or not parsed.hostname
            or not parsed.hostname.endswith(".cognitiveservices.azure.com")):
        raise ValueError("invalid_speech_endpoint")
    return endpoint.rstrip("/") + "/speechtotext/transcriptions:transcribe?api-version=2025-10-15"


def synthetic_wav():
    audio = io.BytesIO()
    with wave.open(audio, "wb") as writer:
        writer.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        writer.writeframes(b"\x00" * 32000)
    return audio.getvalue()


def probe(http, identity, endpoint):
    token = identity.get_token("https://cognitiveservices.azure.com/.default").token
    started = time.monotonic()
    response = http.post(
        endpoint_url(endpoint),
        headers={"Authorization": "Bearer " + token},
        files={
            "audio": ("synthetic-silence.wav", synthetic_wav(), "audio/wav"),
            "definition": (None, '{"locales":[]}', "application/json"),
        },
    )
    if response.status_code != 200:
        print(json.dumps({"event": "speech_probe_rejected", "http_status": response.status_code}), flush=True)
        raise ValueError("speech_request_rejected")
    body = response.json()
    if (not isinstance(body, dict)
            or type(body.get("durationMilliseconds")) is not int
            or not 0 <= body["durationMilliseconds"] <= 1100
            or not isinstance(body.get("combinedPhrases"), list)
            or not isinstance(body.get("phrases"), list)):
        raise ValueError("invalid_speech_response")
    print(json.dumps({
        "event": "speech_probe_succeeded",
        "elapsed_seconds": round(time.monotonic() - started, 2),
        "duration_ms": body["durationMilliseconds"],
        "phrase_count": len(body["phrases"]),
        "api_version": "2025-10-15",
        "input": "one_second_synthetic_silence",
    }), flush=True)


def main():
    client_id = str(UUID(os.environ["AZURE_CLIENT_ID"]))
    endpoint = os.environ["SPEECH_ENDPOINT"]
    endpoint_url(endpoint)
    with ManagedIdentityCredential(client_id=client_id) as identity:
        with httpx.Client(timeout=120, follow_redirects=False, trust_env=False) as http:
            probe(http, identity, endpoint)


if __name__ == "__main__":
    try:
        main()
    except (httpx.HTTPError, AzureError, ValueError, KeyError, TypeError):
        print('{"event":"speech_probe_failed","reason":"identity_transport_or_response_error"}', flush=True)
        raise SystemExit(1) from None
