import importlib.util
import io
import json
from pathlib import Path
from types import SimpleNamespace
import wave

import httpx
import pytest

source = Path(__file__).resolve().parents[1] / "scripts" / "probe_speech.py"
spec = importlib.util.spec_from_file_location("speech_probe", source)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def test_silence_is_one_second_of_recorder_audio():
    with wave.open(io.BytesIO(probe.synthetic_wav()), "rb") as audio:
        assert (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getnframes()) == (1, 2, 16000, 16000)
        assert audio.readframes(16000) == b"\x00" * 32000


@pytest.mark.parametrize("endpoint", [
    "http://recorder.cognitiveservices.azure.com",
    "https://recorder.cognitiveservices.azure.com.evil.example",
    "https://user:password@recorder.cognitiveservices.azure.com",
    "https://recorder.cognitiveservices.azure.com/other",
    "https://recorder.cognitiveservices.azure.com?key=secret",
])
def test_endpoint_cannot_redirect_bearer(endpoint):
    with pytest.raises(ValueError):
        probe.endpoint_url(endpoint)


def test_fast_transcription_is_multilingual_and_keyless(capsys):
    class Identity:
        def get_token(self, scope):
            assert scope == "https://cognitiveservices.azure.com/.default"
            return SimpleNamespace(token="TEST_SENSITIVE_TOKEN")

    class HTTP:
        def post(self, url, headers, files):
            assert url.endswith("/speechtotext/transcriptions:transcribe?api-version=2025-10-15")
            assert headers == {"Authorization": "Bearer TEST_SENSITIVE_TOKEN"}
            assert json.loads(files["definition"][1]) == {"locales": []}
            return httpx.Response(200, json={
                "durationMilliseconds": 1000, "combinedPhrases": [], "phrases": [],
            })

    probe.probe(HTTP(), Identity(), "https://recorder.cognitiveservices.azure.com")
    output = capsys.readouterr().out
    assert "speech_probe_succeeded" in output
    assert "TEST_SENSITIVE" not in output


def test_provider_errors_do_not_print_credentials(capsys):
    class HTTP:
        def post(self, *args, **kwargs):
            return httpx.Response(403, json={"message": "TEST_SENSITIVE_PROVIDER_MESSAGE"})

    identity = SimpleNamespace(get_token=lambda scope: SimpleNamespace(token="TEST_SENSITIVE_TOKEN"))
    with pytest.raises(ValueError, match="speech_request_rejected"):
        probe.probe(HTTP(), identity, "https://recorder.cognitiveservices.azure.com")
    output = capsys.readouterr().out
    assert '"http_status": 403' in output
    assert "TEST_SENSITIVE" not in output
