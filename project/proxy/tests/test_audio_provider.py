import base64
import json
from dataclasses import replace
from types import SimpleNamespace
from unittest.mock import AsyncMock

import numpy as np
import pytest

from recorder_proxy.audio import AudioError, OutputAudio
from recorder_proxy.providers import ProviderError, VoiceLive, map_event, session_configuration, verify_configuration
import recorder_proxy.providers as providers_module


def convert(samples, chunk):
    output = OutputAudio()
    packets = []
    for index in range(0, len(samples), chunk):
        packets.extend(output.feed(samples[index:index + chunk].astype("<i2").tobytes()))
    packets.extend(output.feed(final=True))
    assert all(2 <= len(packet) <= 640 and len(packet) % 2 == 0 for packet in packets)
    return np.frombuffer(b"".join(packets), dtype="<i2")


def test_resampler_continuity_duration_and_gain():
    samples = (np.sin(np.arange(24000) * 2 * np.pi * 1000 / 24000) * 12000).astype(np.int16)
    whole = convert(samples, 24000)
    chunked = convert(samples, 137)
    assert len(whole) == len(chunked) == 16000
    # libsoxr dithers int16 output; independent streams can differ by two LSBs.
    np.testing.assert_allclose(whole, chunked, atol=2, rtol=0)
    assert abs(np.sqrt(np.mean(whole[100:-100].astype(float) ** 2)) - 12000 / np.sqrt(2)) < 10


def test_resampler_antialias():
    samples = (np.sin(np.arange(24000) * 2 * np.pi * 10000 / 24000) * 20000).astype(np.int16)
    output = convert(samples, 480)
    assert np.sqrt(np.mean(output[100:-100].astype(float) ** 2)) < 20


def test_passthrough_short_tail_and_finish():
    audio = OutputAudio(16000)
    assert audio.feed(bytes(638)) == []
    assert audio.feed(b"\x01\x00", final=True) == [bytes(638) + b"\x01\x00"]
    with pytest.raises(AudioError):
        audio.feed()
    with pytest.raises(AudioError):
        OutputAudio().feed(b"\0")


def test_provider_configuration(settings):
    data = session_configuration()
    assert data["session"]["turn_detection"]["interrupt_response"] is True
    assert data["session"]["turn_detection"]["create_response"] is False
    assert data["session"]["tools"] == []
    assert data["session"]["tool_choice"] == "none"
    assert data["session"]["input_audio_sampling_rate"] == 16000
    accepted = {"session": {**data["session"], "model": settings.model}}
    verify_configuration(accepted, settings)
    for key in ("tools", "input_audio_sampling_rate", "model", "turn_detection", "tool_choice"):
        altered = {"session": dict(accepted["session"])}
        del altered["session"][key]
        with pytest.raises(ProviderError):
            verify_configuration(altered, settings)


@pytest.fixture
def observed_session_updated():
    return {
        "type": "session.updated",
        "session": {
            "model": "gpt-realtime-2-global-standard",
            "input_audio_format": "pcm16",
            "input_audio_sampling_rate": 16000,
            "output_audio_format": "pcm16",
            "input_audio_echo_cancellation": {
                "type": "server_echo_cancellation", "reference_source": "server", "channels": 1,
            },
            "turn_detection": {
                "type": "azure_semantic_vad_multilingual",
                "create_response": False, "interrupt_response": True,
                "threshold": 0.5, "prefix_padding_ms": 420, "silence_duration_ms": 500,
                "idle_timeout_ms": None, "profile": "classic", "neg_threshold": None,
                "speech_duration_ms": None, "window_size": None, "distinct_ci_phones": None,
                "require_vowel": None, "require_vowel_assistant_speaking": None,
                "remove_filler_words": False, "languages": None, "auto_truncate": False,
                "appended_text_after_truncation": None,
            },
            "tools": [], "tool_choice": "none", "modalities": ["audio", "text"],
        },
    }


def test_observed_native_session_accepts_canonical_alias_and_service_defaults(settings, observed_session_updated):
    verify_configuration(observed_session_updated, settings)
    assert settings.model == "gpt-realtime-2"
    echo = observed_session_updated["session"]["input_audio_echo_cancellation"]
    echo["service_default_extension"] = 0
    verify_configuration(observed_session_updated, settings)


@pytest.mark.parametrize("field,value", [
    ("type", "client_echo_cancellation"), ("type", None),
    ("reference_source", "client"), ("reference_source", None),
    ("channels", 2), ("channels", 0), ("channels", True), ("channels", "1"),
    ("channels", 1.0), ("channels", None),
])
def test_observed_echo_rejects_non_server_or_non_mono(settings, observed_session_updated, field, value):
    observed_session_updated["session"]["input_audio_echo_cancellation"][field] = value
    with pytest.raises(ProviderError):
        verify_configuration(observed_session_updated, settings)


@pytest.mark.parametrize("model", [
    "gpt-realtime-2-mini", "gpt-realtime", "gpt-realtime-2-global-standard-extra",
    "gpt-realtime-2-global-standard-mini", "gpt-realtime-2-global-provisioned",
    "GPT-REALTIME-2-GLOBAL-STANDARD", None, ["gpt-realtime-2-global-standard"],
])
def test_observed_native_alias_is_exact(settings, observed_session_updated, model):
    observed_session_updated["session"]["model"] = model
    with pytest.raises(ProviderError):
        verify_configuration(observed_session_updated, settings)


@pytest.mark.parametrize("model", ["gpt-realtime-2-mini", "gpt-realtime-2-extra", "another-model"])
def test_native_alias_cannot_satisfy_a_different_requested_model(settings, observed_session_updated, model):
    with pytest.raises(ProviderError):
        verify_configuration(observed_session_updated, replace(settings, model=model))


@pytest.mark.parametrize("model", ["gpt-realtime-2", "my-realtime-deployment"])
def test_byom_requires_exact_deployment_identity(settings, observed_session_updated, model):
    settings = replace(settings, profile="byom-azure-openai-realtime", model=model)
    with pytest.raises(ProviderError):
        verify_configuration(observed_session_updated, settings)
    observed_session_updated["session"]["model"] = model
    verify_configuration(observed_session_updated, settings)


@pytest.mark.parametrize("field,value", [
    ("create_response", True), ("create_response", 0),
    ("interrupt_response", False), ("interrupt_response", 1),
    ("type", "server_vad"),
])
def test_observed_vad_required_fields_remain_strict(settings, observed_session_updated, field, value):
    observed_session_updated["session"]["turn_detection"][field] = value
    with pytest.raises(ProviderError):
        verify_configuration(observed_session_updated, settings)


def test_audio_mapping():
    data = {"type": "response.audio.delta", "response_id": "r1", "item_id": "i1",
            "content_index": 0, "delta": base64.b64encode(b"\1\0" * 3).decode()}
    event = map_event(data)
    assert event.type == "audio" and event.pcm == b"\1\0" * 3
    assert map_event({"type": "input_audio_buffer.speech_started"}).type == "speech_started"
    data["type"] = "response.audio.done"
    assert map_event(data).type == "audio_done"
    assert map_event({"type": "response.audio_transcript.delta", "delta": "private"}) is None


@pytest.mark.parametrize("data", [
    {"type": "error", "error": {"message": "secret audio"}},
    {"type": "response.output_audio.delta"},  # Direct OpenAI GA is not Voice Live v1.
    {"type": "response.function_call_arguments.done"},
    {"type": "response.output_item.added", "item": {"type": "function_call"}},
    {"type": "response.done", "response": {"status": "failed"}},
    {"type": "response.audio.delta", "response_id": "r", "item_id": "i", "content_index": 0, "delta": "***"},
    {"type": "response.audio.delta", "response_id": "r", "item_id": "i", "content_index": 0, "delta": "AA=="},
    {"type": "response.audio.delta", "response_id": "r", "item_id": "i", "content_index": True, "delta": "AAA="},
])
def test_provider_error_redaction(data):
    with pytest.raises(ProviderError) as exc:
        map_event(data)
    assert "secret" not in str(exc.value)


async def test_append_truncate_and_close(settings):
    provider = VoiceLive(settings)
    await provider.credential.close()
    provider.credential = SimpleNamespace(close=AsyncMock())
    provider.socket = SimpleNamespace(send=AsyncMock(), close=AsyncMock())
    await provider.append(b"\1\0")
    await provider.truncate("item", 0, 12345)
    append, truncate = [json.loads(call.args[0]) for call in provider.socket.send.call_args_list]
    assert append == {"type": "input_audio_buffer.append", "audio": "AQA="}
    assert truncate == {"type": "conversation.item.truncate", "item_id": "item",
                        "content_index": 0, "audio_end_ms": 771}
    await provider.respond()
    assert json.loads(provider.socket.send.call_args.args[0]) == {"type": "response.create"}
    await provider.close()
    provider.socket.close.assert_awaited_once()
    provider.credential.close.assert_awaited_once()


@pytest.mark.parametrize("profile", ["native", "byom-azure-openai-realtime"])
async def test_raw_connection_contract_and_accepted_configuration(settings, monkeypatch, profile):
    settings = replace(settings, profile=profile)
    accepted = {"type": "session.updated",
                "session": {**session_configuration()["session"], "model": settings.model}}
    socket = SimpleNamespace(
        recv=AsyncMock(side_effect=[json.dumps({"type": "session.created"}), json.dumps(accepted)]),
        send=AsyncMock(), close=AsyncMock(),
    )
    connection = AsyncMock(return_value=socket)
    monkeypatch.setattr(providers_module, "connect", connection)
    provider = VoiceLive(settings)
    await provider.credential.close()
    provider.credential = SimpleNamespace(
        get_token=AsyncMock(return_value=SimpleNamespace(token="synthetic-MI-token")),
        close=AsyncMock(),
    )
    await provider.open()
    url = connection.call_args.args[0]
    kwargs = connection.call_args.kwargs
    assert url.startswith("wss://unit-test.services.ai.azure.com/voice-live/realtime?")
    assert "api-version=2026-07-15" in url and "model=gpt-realtime-2" in url
    assert ("profile=byom-azure-openai-realtime" in url) == (profile != "native")
    assert kwargs["additional_headers"] == {"Authorization": "Bearer synthetic-MI-token"}
    assert kwargs["max_size"] == 131072 and kwargs["max_queue"] == 4
    assert kwargs["ping_timeout"] == 10 and kwargs["compression"] is None
    assert "synthetic-MI-token" not in url
    assert json.loads(socket.send.call_args.args[0]) == session_configuration()
    provider.credential.get_token.assert_awaited_once_with("https://ai.azure.com/.default")
    await provider.close()


async def test_open_accepts_observed_native_session(settings, monkeypatch, observed_session_updated):
    socket = SimpleNamespace(
        recv=AsyncMock(side_effect=[
            json.dumps({"type": "session.created"}), json.dumps(observed_session_updated),
        ]),
        send=AsyncMock(), close=AsyncMock(),
    )
    connection = AsyncMock(return_value=socket)
    monkeypatch.setattr(providers_module, "connect", connection)
    provider = VoiceLive(settings)
    await provider.credential.close()
    provider.credential = SimpleNamespace(
        get_token=AsyncMock(return_value=SimpleNamespace(token="synthetic-MI-token")),
        close=AsyncMock(),
    )
    try:
        await provider.open()
        assert "model=gpt-realtime-2&" in connection.call_args.args[0] + "&"
        assert "gpt-realtime-2-global-standard" not in connection.call_args.args[0]
    finally:
        await provider.close()


async def test_observed_audio_response_event_sequence(settings):
    item = {"id": "i1", "type": "message", "role": "assistant",
            "content": [{"type": "audio", "transcript": "Hello."}]}
    identity = {"response_id": "r1", "item_id": "i1", "content_index": 0}
    wire_events = [
        {"type": "response.created", "response": {"id": "r1"}},
        {"type": "response.output_item.added", "item": item},
        {"type": "conversation.item.created", "item": item},
        {"type": "response.content_part.added", **identity, "part": {"type": "audio"}},
        {"type": "response.audio_transcript.delta", **identity, "delta": "Hello."},
        {"type": "response.audio.delta", **identity, "delta": base64.b64encode(bytes(960)).decode()},
        {"type": "response.audio.done", **identity},
        {"type": "response.audio_transcript.done", **identity, "transcript": "Hello."},
        {"type": "response.content_part.done", **identity, "part": {"type": "audio"}},
        {"type": "response.output_item.done", "item": item},
        {"type": "response.done", "response": {"id": "r1", "status": "completed", "output": [item]}},
    ]
    provider = VoiceLive(settings)
    await provider.credential.close()
    provider.credential = SimpleNamespace(close=AsyncMock())
    provider.socket = SimpleNamespace(
        recv=AsyncMock(side_effect=[json.dumps(event) for event in wire_events]),
        close=AsyncMock(),
    )
    try:
        events = []
        async for event in provider.events():
            events.append(event)
            if event.type == "response_done":
                break
        assert [event.type for event in events] == ["response_started", "audio", "audio_done", "response_done"]
        assert events[1].pcm == bytes(960)
    finally:
        await provider.close()


async def test_raw_connection_network_failure_redacted(settings, monkeypatch):
    connection = AsyncMock(side_effect=OSError("secret provider text"))
    monkeypatch.setattr(providers_module, "connect", connection)
    provider = VoiceLive(settings)
    await provider.credential.close()
    provider.credential = SimpleNamespace(
        get_token=AsyncMock(return_value=SimpleNamespace(token="synthetic-MI-token")),
        close=AsyncMock(),
    )
    with pytest.raises(ProviderError) as exc:
        await provider.open()
    assert not str(exc.value)
    await provider.close()
