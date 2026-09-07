import asyncio
from dataclasses import replace
import hashlib
import io
import json
from pathlib import Path
import struct

import httpx
import pytest

from recorder_proxy.graph import GraphClient
from recorder_proxy.intelligence_errors import IntelligenceError
from recorder_proxy.obo import UserContext
from recorder_proxy.processing import RecordingProcessor, validate_wav
from recorder_proxy.recording_contract import MAX_WAV_BYTES, RecordingRequest
from recorder_proxy.speech import FastTranscription, SPEECH_SCOPE
from intelligence_fakes import Credential, Drive, SpeechService, Tokens, wav_bytes


@pytest.fixture
async def processing(settings, principal, monkeypatch):
    monkeypatch.chdir(Path(__file__).resolve().parents[1])
    settings = replace(settings, recording_processing_enabled=True,
                       speech_endpoint="https://unit.cognitiveservices.azure.com")
    drive, service, credential = Drive(), SpeechService(), Credential()
    graph_http = httpx.AsyncClient(transport=httpx.MockTransport(drive.handle))
    speech = FastTranscription(settings, credential=credential,
                               http=httpx.AsyncClient(transport=httpx.MockTransport(service.handle)))
    factory = lambda user: GraphClient(settings, Tokens(), user, http=graph_http)
    processor = RecordingProcessor(settings, factory, speech)
    request = RecordingRequest("drive", "audio", hashlib.sha1(drive.content["audio"]).hexdigest(),
                               len(drive.content["audio"]), "2026-09-07T22:05:36+08:00")
    user = UserContext(principal, "validated-B")
    yield processor, request, user, drive, service, credential
    assert not list(Path.cwd().glob(".recording-*.wav"))
    await speech.close()
    await graph_http.aclose()


async def test_process_real_graph_speech_multipart_and_restart_status(processing):
    processor, request, user, drive, speech, credential = processing
    before = drive.content["audio"]
    assert (await processor.handle(user, request, status=True))["status"] == "not_started"
    result = await processor.handle(user, request)
    assert result["status"] == "completed"
    assert len(result["operation_id"]) == 64
    assert drive.writes == ["AudioRecording_20260907_220536.txt",
                            "AudioRecording_20260907_220536.transcription.json"]
    assert drive.content["audio"] == before
    document = json.loads(drive.content[result["json_item_id"]])
    assert document["source"]["sha1"] == request.source_sha1
    assert document["source"]["recorded_at"] == request.recorded_at
    assert document["pipeline"]["language"] == "auto-multilingual"
    assert document["phrases"][0]["text"] == "你好 hello."
    assert document["phrases"][0]["offset_ms"] == 0
    assert document["no_speech"] is False
    assert "validated-B" not in str(document)
    assert credential.scopes == [SPEECH_SCOPE]
    assert (await processor.handle(user, request))["status"] == "already_completed"
    restarted = RecordingProcessor(processor.settings, processor.graph_factory, processor.speech)
    assert (await restarted.handle(user, request, status=True))["status"] == "already_completed"
    assert len(speech.calls) == 1


async def test_no_speech_is_valid_completed_empty_transcript(processing):
    processor, request, user, drive, speech, _ = processing
    speech.document["phrases"] = []
    result = await processor.handle(user, request)
    document = json.loads(drive.content[result["json_item_id"]])
    assert document["no_speech"] and document["phrases"] == [] and document["text"] == ""
    assert "(No speech recognized.)" in drive.content[result["text_item_id"]].decode()


async def test_partial_write_retries_without_retranscription_or_overwrite(processing):
    processor, request, user, drive, speech, _ = processing
    drive.fail_json = True
    with pytest.raises(IntelligenceError, match="temporarily_unavailable"):
        await processor.handle(user, request)
    assert len(drive.writes) == 1
    assert (await processor.handle(user, request, status=True))["status"] == "retry_required"
    drive.fail_json = False
    result = await processor.handle(user, request)
    assert result["status"] == "completed"
    assert len(speech.calls) == 1 and len(drive.writes) == 2


async def test_conflicting_user_sidecars_are_never_overwritten(processing):
    processor, request, user, drive, speech, _ = processing
    stem = drive.items["audio"]["name"][:-4]
    drive.add("manual", stem + ".txt", "folder", b"My personal notes.")
    with pytest.raises(IntelligenceError, match="output_conflict"):
        await processor.handle(user, request)
    assert drive.content["manual"] == b"My personal notes."
    assert not speech.calls and not drive.writes


async def test_completed_marker_with_missing_or_modified_text_fails_closed(processing):
    processor, request, user, drive, _, _ = processing
    result = await processor.handle(user, request)
    id = result["text_item_id"]
    drive.content[id] = drive.content[id].replace(b"hello", b"other")
    with pytest.raises(IntelligenceError, match="output_conflict"):
        await processor.handle(user, request, status=True)
    del drive.items[id]
    with pytest.raises(IntelligenceError, match="output_conflict"):
        await processor.handle(user, request)


async def test_source_changes_during_speech_never_commits_outputs(processing):
    processor, request, user, drive, speech, _ = processing
    speech.wait = asyncio.Event()
    work = asyncio.create_task(processor.handle(user, request))
    while not speech.calls:
        await asyncio.sleep(0)
    drive.items["audio"]["eTag"] = '"changed"'
    speech.wait.set()
    with pytest.raises(IntelligenceError, match="source_changed"):
        await work
    assert not drive.writes


async def test_no_queue_duplicate_status_and_cancellation_cleanup(processing):
    processor, request, user, drive, speech, _ = processing
    speech.wait = asyncio.Event()
    work = asyncio.create_task(processor.handle(user, request))
    while not speech.calls:
        await asyncio.sleep(0)
    assert (await processor.handle(user, request, status=True))["status"] == "processing"
    with pytest.raises(IntelligenceError, match="processing_in_progress"):
        await processor.handle(user, request)
    with pytest.raises(IntelligenceError, match="busy"):
        await processor.handle(user, replace(request, item_id="other"))
    work.cancel()
    with pytest.raises(asyncio.CancelledError):
        await work
    assert processor.running is None and not drive.writes
    assert not list(Path.cwd().glob(".recording-*.wav"))


async def test_deadline_cancels_speech_and_releases_slot(processing):
    processor, request, user, drive, speech, _ = processing
    processor.settings = replace(processor.settings, processing_deadline_seconds=0.05)
    speech.wait = asyncio.Event()
    with pytest.raises(IntelligenceError, match="processing_deadline") as exc:
        await processor.handle(user, request)
    assert exc.value.status == 504 and exc.value.retryable
    assert processor.running is None and not drive.writes


@pytest.mark.parametrize("status,code", [(429, "busy"), (500, "temporarily_unavailable"),
                                      (400, "unsupported_audio"), (413, "recording_too_long")])
async def test_speech_stable_errors_preserve_input(processing, status, code):
    processor, request, user, drive, speech, _ = processing
    speech.status = status
    with pytest.raises(IntelligenceError, match=code):
        await processor.handle(user, request)
    assert not drive.writes and drive.content["audio"]


async def test_actual_hash_is_authoritative_when_graph_hash_missing(processing):
    processor, request, user, drive, speech, _ = processing
    drive.items["audio"]["file"] = {}
    with pytest.raises(IntelligenceError, match="source_changed"):
        await processor.handle(user, replace(request, source_sha1="a" * 40))
    assert not speech.calls


@pytest.mark.parametrize("content", [
    b"not a wave", wav_bytes(rate=8000), wav_bytes(channels=2), wav_bytes(width=1),
    wav_bytes()[:-1], wav_bytes() + b"unexpected", b"RIFF" + struct.pack("<I", 4) + b"WAVE",
])
async def test_bad_wave_is_not_sent_to_speech(processing, content):
    processor, request, user, drive, speech, _ = processing
    drive.add("audio", drive.items["audio"]["name"], "folder", content)
    request = replace(request, source_sha1=hashlib.sha1(content).hexdigest(), source_size=len(content))
    with pytest.raises(IntelligenceError, match="unsupported_audio"):
        await processor.handle(user, request)
    assert not speech.calls and not drive.writes


def test_duration_uses_actual_data_chunk_and_size_limit():
    data = wav_bytes()
    assert MAX_WAV_BYTES < 58 * 1024 * 1024
    # Validate a sparse project-local WAV without constructing 57.6 MB in RAM.
    path = Path(__file__).resolve().parent / ".duration-test.wav"
    try:
        with path.open("w+b") as file:
            size = 1800 * 32000 + 2
            header = bytearray(data[:44])
            struct.pack_into("<I", header, 4, size + 36)
            struct.pack_into("<I", header, 40, size)
            file.write(header)
            file.seek(size + 43)
            file.write(b"\0")
            file.flush()
            with pytest.raises(IntelligenceError, match="recording_too_long"):
                validate_wav(file)
    finally:
        path.unlink(missing_ok=True)


@pytest.mark.parametrize("changes,code", [
    ({"v": True}, "invalid_request"), ({"v": 2}, "invalid_request"),
    ({"user_id": "injected"}, "invalid_request"), ({"source_size": True}, "invalid_request"),
    ({"source_size": MAX_WAV_BYTES + 1}, "recording_too_long"),
    ({"source_sha1": "A" * 40}, "invalid_request"),
    ({"recorded_at": "2026-09-07"}, "invalid_request"),
    ({"recorded_at": "2026-99-07T12:00:00Z"}, "invalid_request"),
    ({"download_url": "https://evil"}, "invalid_request"),
])
def test_contract_rejects_untrusted_overrides(changes, code):
    body = {"v": 1, "drive_id": "drive", "item_id": "audio",
            "source_sha1": "a" * 40, "source_size": 44, **changes}
    with pytest.raises(IntelligenceError, match=code):
        RecordingRequest.parse(body)


@pytest.mark.parametrize("document", [
    {}, {"durationMilliseconds": 10, "combinedPhrases": [], "phrases": None},
    {"durationMilliseconds": True, "combinedPhrases": [], "phrases": []},
    {"durationMilliseconds": 10, "combinedPhrases": [{"text": None}], "phrases": []},
    {"durationMilliseconds": 10, "combinedPhrases": [], "phrases": [
        {"offsetMilliseconds": 0, "durationMilliseconds": -1, "text": "x"}]},
])
async def test_malformed_speech_is_not_reported_as_no_speech(processing, document):
    processor, request, user, drive, speech, _ = processing
    speech.document = document
    with pytest.raises(IntelligenceError, match="temporarily_unavailable"):
        await processor.handle(user, request)
    assert not drive.writes


async def test_manual_edit_of_partial_txt_is_a_conflict(processing):
    processor, request, user, drive, _, _ = processing
    drive.fail_json = True
    with pytest.raises(IntelligenceError):
        await processor.handle(user, request)
    drive.content["output-0"] = drive.content["output-0"].replace(b"hello", b"other")
    drive.fail_json = False
    with pytest.raises(IntelligenceError, match="output_conflict"):
        await processor.handle(user, request)


def test_operation_is_server_derived_owner_source_and_options_bound():
    request = RecordingRequest("drive", "audio", "a" * 40, 44)
    assert request.operation("owner") == replace(request, recorded_at="2026-09-07T00:00:00Z").operation("owner")
    assert len({request.operation("owner"), request.operation("other"),
                replace(request, source_sha1="b" * 40).operation("owner"),
                replace(request, item_id="different").operation("owner")}) == 4


def test_status_operation_identity_and_completion_ids_fit_device_bounds():
    for status in ("not_started", "processing", "retry_required"):
        result = RecordingProcessor.result("a" * 64, status)
        assert result == {"v": 1, "operation_id": "a" * 64, "status": status}
    for status in ("completed", "already_completed"):
        result = RecordingProcessor.result("a" * 64, status, {"id": "j" * 128}, {"id": "t" * 128})
        assert len(result["json_item_id"]) == len(result["text_item_id"]) == 128
        with pytest.raises(IntelligenceError, match="temporarily_unavailable"):
            RecordingProcessor.result("a" * 64, status, {"id": "j" * 129}, {"id": "text"})
