import asyncio
from contextlib import ExitStack
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
import json
from pathlib import Path
import socket
from types import SimpleNamespace

import httpx
import pytest
from starlette.testclient import WebSocketDenialResponse
from starlette.websockets import WebSocketDisconnect
import uvicorn
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed

from recorder_proxy import processing_stream
from recorder_proxy.intelligence_errors import IntelligenceError
from recorder_proxy.processing_stream import ProcessingStream, SUBPROTOCOL
from recorder_proxy.speech import STREAM_READ_TIMEOUT_SECONDS
from intelligence_fakes import wav_bytes
from test_app import deployment
from test_auth import token
from test_processing import processing
from test_processing_http import api
from test_sessions import HELLO

PATH = "/v1/recordings/process-stream"


def open_stream(client, authorization, **kwargs):
    return client.websocket_connect(
        PATH, headers={"Authorization": authorization}, subprotocols=[SUBPROTOCOL], **kwargs)


def terminal(socket):
    messages = []
    while True:
        message = socket.receive_json()
        assert len(json.dumps(message).encode()) <= 4096
        messages.append(message)
        if message["type"] in ("result", "error"):
            return messages


def test_stream_completes_and_http_reconciles_without_retranscribing(api):
    client, body, key, claims, drive, service = api
    drive.redirect = "https://my.microsoftpersonalcontent.com/personal/fixture/download?tempauth=TEST_ONLY"
    authorization = token(key, claims)
    for expected in ("completed", "already_completed"):
        with open_stream(client, authorization) as socket:
            assert socket.accepted_subprotocol == SUBPROTOCOL
            socket.send_json(body)
            messages = terminal(socket)
            assert messages[0]["type"] == "started"
            result = messages[-1]
            assert result["status"] == expected
            assert result["operation_id"] == messages[0]["operation_id"]
            assert result["json_item_id"] in drive.content
            assert result["text_item_id"] in drive.content
            assert "你好" not in json.dumps(messages, ensure_ascii=False)
            if expected == "completed":
                phases = {m["phase"] for m in messages if m["type"] == "progress"}
                assert phases and phases <= processing_stream.PHASES
                transfers = [m for m in messages if "completed_bytes" in m]
                assert transfers
                assert all(0 <= m["completed_bytes"] <= m["total_bytes"] == body["source_size"]
                           for m in transfers)
    status = client.post("/v1/recordings/status", json=body, headers={"Authorization": authorization})
    assert status.json()["status"] == "already_completed"
    assert len(service.calls) == 1
    assert service.calls[0].extensions["timeout"] == {
        "connect": 5, "read": STREAM_READ_TIMEOUT_SECONDS, "write": 30, "pool": 5}


@pytest.mark.parametrize("change", [
    {"aud": "00000003-0000-0000-c000-000000000000"}, {"azp": "wrong"},
    {"oid": "wrong"}, {"tid": "wrong"}, {"exp": 1}, {"scp": "Files.ReadWrite"},
])
def test_stream_validates_b_before_accept(api, change):
    client, _, key, claims, drive, service = api
    with pytest.raises(WebSocketDenialResponse) as exc:
        with open_stream(client, token(key, {**claims, **change})):
            pytest.fail("Unauthorized upgrade")
    assert exc.value.status_code == 401
    assert not drive.calls and not service.calls


@pytest.mark.parametrize("kind,status", [
    ("missing", 401), ("empty", 401), ("duplicate", 401),
    ("query", 400), ("protocol", 400), ("voice_protocol", 400),
])
def test_stream_upgrade_strict_headers_and_protocol(api, kind, status):
    client, _, key, claims, drive, service = api
    auth = token(key, claims)
    headers, protocols, path = {"Authorization": auth}, [SUBPROTOCOL], PATH
    if kind == "missing":
        headers = {}
    elif kind == "empty":
        headers = {"Authorization": "Bearer "}
    elif kind == "duplicate":
        headers = httpx.Headers([("Authorization", auth), ("Authorization", auth)])
    elif kind == "query":
        path += "?token=private"
    elif kind == "protocol":
        protocols = []
    elif kind == "voice_protocol":
        protocols = ["recorder.voice.v1"]
    with pytest.raises(WebSocketDenialResponse) as exc:
        with client.websocket_connect(path, headers=headers, subprotocols=protocols):
            pytest.fail("Invalid upgrade")
    assert exc.value.status_code == status
    assert not drive.calls and not service.calls


@pytest.mark.parametrize("raw", [
    "x" * 4097, "é" * 2049, '{"v":1,"v":1}', "[]", '{"v":NaN}', "{}",
    '{"v":true}', '{"v":2}', '{"v":1,"download_url":"https://example.invalid"}',
])
def test_stream_initial_request_bounds_and_schema(api, raw):
    client, _, key, claims, drive, service = api
    with open_stream(client, token(key, claims)) as socket:
        socket.send_text(raw)
        assert terminal(socket)[-1] == {
            "v": 1, "type": "error", "error": {"code": "invalid_request", "retryable": False}}
    assert not drive.calls and not service.calls


def test_stream_binary_request_rejected(api):
    client, _, key, claims, drive, service = api
    with open_stream(client, token(key, claims)) as socket:
        socket.send_bytes(b'{"v":1}')
        assert terminal(socket)[-1]["error"]["code"] == "invalid_request"
    assert not drive.calls and not service.calls


def test_stream_initial_deadline(api, monkeypatch):
    client, _, key, claims, drive, service = api
    monkeypatch.setattr(processing_stream, "INITIAL_REQUEST_SECONDS", 0.01)
    with open_stream(client, token(key, claims)) as socket:
        assert terminal(socket)[-1]["error"]["code"] == "invalid_request"
    assert not drive.calls and not service.calls


def test_processing_stream_feature_disabled_is_denied_before_upgrade(deployment):
    client, key, claims, providers = deployment
    with pytest.raises(WebSocketDenialResponse) as exc:
        with open_stream(client, token(key, claims)):
            pytest.fail("Disabled processing upgrade")
    assert exc.value.status_code == 503
    assert not providers


def test_stream_token_expiry_also_bounds_initial_request_wait(api, monkeypatch):
    client, _, key, claims, drive, service = api
    monkeypatch.setattr(processing_stream, "time",
                        SimpleNamespace(time=lambda: claims["exp"] - 0.01))
    with open_stream(client, token(key, claims)) as socket:
        assert terminal(socket)[-1]["error"]["code"] == "authentication_required"
    assert not drive.calls and not service.calls


def test_stream_pending_requests_share_http_limit_not_voice_slot(api):
    client, body, key, claims, drive, service = api
    auth = token(key, claims)
    with ExitStack() as stack:
        for _ in range(8):
            stack.enter_context(open_stream(client, auth))
        with pytest.raises(WebSocketDenialResponse) as exc:
            with open_stream(client, auth):
                pytest.fail("Unbounded socket admission")
        assert exc.value.status_code == 429
        assert client.post("/v1/recordings/status", json=body,
                           headers={"Authorization": auth}).status_code == 429
        with client.websocket_connect("/v1/voice", headers={"Authorization": auth},
                                      subprotocols=["recorder.voice.v1"]) as voice:
            voice.send_json(HELLO)
            assert voice.receive_json()["state"] == "connecting"
            assert voice.receive_json()["type"] == "ready"
            voice.send_json({"v": 1, "type": "stop"})
    assert not drive.calls and not service.calls


def test_stream_heartbeat_duplicate_http_and_ws_voice_remains_independent(api, monkeypatch):
    client, body, key, claims, drive, service = api
    service.wait = asyncio.Event()
    monkeypatch.setattr(processing_stream, "HEARTBEAT_SECONDS", 0.01)
    auth = token(key, claims)
    with open_stream(client, auth) as first:
        first.send_json(body)
        while True:
            message = first.receive_json()
            if message["type"] == "heartbeat" and message["phase"] == "transcribing":
                assert set(message) == {"v", "type", "phase"}
                break
        assert len(service.calls) == 1
        headers = {"Authorization": auth}
        assert client.post("/v1/recordings/status", json=body,
                           headers=headers).json()["status"] == "processing"
        assert client.post("/v1/recordings/process", json=body,
                           headers=headers).json()["error"]["code"] == "processing_in_progress"
        for request, code in ((body, "processing_in_progress"), ({**body, "item_id": "other"}, "busy")):
            with open_stream(client, auth) as duplicate:
                duplicate.send_json(request)
                assert terminal(duplicate)[-1]["error"]["code"] == code
        with client.websocket_connect("/v1/voice", headers=headers,
                                      subprotocols=["recorder.voice.v1"]) as voice:
            voice.send_json(HELLO)
            assert voice.receive_json()["state"] == "connecting"
            assert voice.receive_json()["type"] == "ready"
            voice.send_json({"v": 1, "type": "stop"})
        first.send_json({"v": 1, "type": "cancel"})
        with pytest.raises(WebSocketDisconnect):
            while True:
                first.receive_json()
    assert not drive.writes and len(service.calls) == 1
    assert client.post("/v1/recordings/status", json=body,
                       headers=headers).json()["status"] == "not_started"


def test_http_owned_operation_rejects_duplicate_stream_without_cancelling_http(api):
    client, body, key, claims, _, service = api
    service.wait = asyncio.Event()
    auth = token(key, claims)

    async def started():
        async with asyncio.timeout(3):
            while not service.calls:
                await asyncio.sleep(0.001)

    with ThreadPoolExecutor(max_workers=1) as pool:
        result = pool.submit(client.post, "/v1/recordings/process", json=body,
                             headers={"Authorization": auth})
        try:
            client.portal.call(started)
            with open_stream(client, auth) as socket:
                socket.send_json(body)
                assert terminal(socket)[-1]["error"]["code"] == "processing_in_progress"
            assert not result.done()
        finally:
            client.portal.call(service.wait.set)
        assert result.result(timeout=3).json()["status"] == "completed"
    assert len(service.calls) == 1


@pytest.mark.parametrize("command", [
    {"v": 1, "type": "cancel", "extra": 1}, {"v": True, "type": "cancel"},
    {"v": 2, "type": "cancel"}, {"v": 1, "type": "start"}, [], "x" * 4097,
])
def test_only_one_request_and_strict_cancel(api, command):
    client, body, key, claims, drive, service = api
    service.wait = asyncio.Event()
    with open_stream(client, token(key, claims)) as socket:
        socket.send_json(body)
        assert socket.receive_json()["type"] == "started"
        socket.send_json(command)
        assert terminal(socket)[-1]["error"]["code"] == "invalid_request"
    assert not drive.writes


def test_stream_expiry_cancels_speech_and_cleans(api, monkeypatch):
    client, body, key, claims, drive, service = api
    service.wait = asyncio.Event()
    monkeypatch.setattr(processing_stream, "time",
                        SimpleNamespace(time=lambda: claims["exp"] - 0.15))
    auth = token(key, claims)
    with open_stream(client, auth) as socket:
        socket.send_json(body)
        assert terminal(socket)[-1]["error"]["code"] == "authentication_required"
    assert service.calls and not drive.writes
    assert client.post("/v1/recordings/status", json=body,
                       headers={"Authorization": auth}).json()["status"] == "not_started"
    assert not list(Path.cwd().glob(".recording-*.wav"))


def test_stream_disconnect_does_not_leave_operation(api):
    client, body, key, claims, drive, service = api
    service.wait = asyncio.Event()
    auth = token(key, claims)
    with open_stream(client, auth) as socket:
        socket.send_json(body)
        while not service.calls:
            socket.receive_json()
    assert not drive.writes
    assert client.post("/v1/recordings/status", json=body,
                       headers={"Authorization": auth}).json()["status"] == "not_started"


def test_partial_stream_failure_is_recovered_without_second_speech(api, caplog):
    client, body, key, claims, drive, service = api
    drive.fail_json = True
    auth = token(key, claims)
    with open_stream(client, auth) as socket:
        socket.send_json(body)
        assert terminal(socket)[-1]["error"]["code"] == "temporarily_unavailable"
    assert len(drive.writes) == 1
    assert "sensitive provider message" not in caplog.text
    assert auth not in caplog.text and body["source_sha1"] not in caplog.text
    drive.fail_json = False
    with open_stream(client, auth) as socket:
        socket.send_json(body)
        assert terminal(socket)[-1]["status"] == "completed"
    assert len(service.calls) == 1 and len(drive.writes) == 2


async def test_stream_outlasts_compressed_http_budget_without_total_deadline(processing):
    processor, request, user, drive, service, _ = processing
    processor.settings = replace(processor.settings, processing_deadline_seconds=0.01)
    service.wait = asyncio.Event()
    updates = []

    async def progress(phase, **counts):
        updates.append((phase, counts))

    work = asyncio.create_task(processor.handle(user, request, stream=True, progress=progress))
    while not service.calls:
        await asyncio.sleep(0)
    # Compress the old 180-second budget instead of waiting three minutes.
    await asyncio.sleep(0.04)
    assert not work.done()
    service.wait.set()
    assert (await work)["status"] == "completed"
    assert {phase for phase, _ in updates} == processing_stream.PHASES
    assert len(drive.writes) == 2


async def test_http_file_only_speech_fake_interface_is_preserved(processing):
    processor, request, user, _, _, _ = processing

    class Speech:
        async def transcribe(self, file):
            assert not file.closed
            return []

    processor.speech = Speech()
    assert (await processor.handle(user, request))["status"] == "completed"


async def test_stream_speech_fake_receives_optional_interface(processing):
    processor, request, user, _, _, _ = processing

    class Speech:
        async def transcribe(self, file, *, stream=False, progress=None):
            assert stream and not file.closed
            await progress("transcribing")
            return []

    processor.speech = Speech()
    assert (await processor.handle(user, request, stream=True))["status"] == "completed"


async def test_stream_generic_upstream_timeout_is_not_http_operation_deadline(processing):
    processor, request, user, drive, _, _ = processing

    class Speech:
        async def transcribe(self, file, **kwargs):
            raise TimeoutError()

    processor.speech = Speech()
    with pytest.raises(IntelligenceError, match="temporarily_unavailable"):
        await processor.handle(user, request, stream=True)
    assert processor.running is None and not drive.writes


async def test_stream_upstream_read_stall_is_redacted_and_cleans(processing):
    processor, request, user, drive, _, _ = processing

    async def stalled(request):
        assert request.extensions["timeout"]["read"] == 900
        raise httpx.ReadTimeout("PRIVATE_URL_OR_TOKEN")

    await processor.speech.http.aclose()
    processor.speech.http = httpx.AsyncClient(transport=httpx.MockTransport(stalled))
    with pytest.raises(IntelligenceError, match="temporarily_unavailable"):
        await processor.handle(user, request, stream=True)
    assert processor.running is None and not drive.writes


async def test_stream_continuing_download_exceeds_old_budget_with_measured_bytes(processing):
    import hashlib

    processor, request, user, drive, _, _ = processing
    content = wav_bytes(frames=65536)
    drive.add("audio", drive.items["audio"]["name"], "folder", content)
    request = replace(request, source_sha1=hashlib.sha1(content).hexdigest(), source_size=len(content))
    processor.settings = replace(processor.settings, processing_deadline_seconds=0.005)
    original = drive.handle
    updates = []

    class Chunks(httpx.AsyncByteStream):
        async def __aiter__(self):
            for start in range(0, len(content), 65536):
                await asyncio.sleep(0.01)
                yield content[start:start + 65536]

    async def handle(request):
        if request.url.path.endswith("/audio/content"):
            return httpx.Response(200, stream=Chunks())
        return await original(request)

    async def progress(phase, **counts):
        if phase == "downloading":
            updates.append(counts["completed_bytes"])

    graph_http = httpx.AsyncClient(transport=httpx.MockTransport(handle))
    factory = processor.graph_factory

    def graph(user):
        value = factory(user)
        value.http = graph_http
        return value

    processor.graph_factory = graph
    try:
        assert (await processor.handle(user, request, stream=True, progress=progress))["status"] == "completed"
        assert updates == [0, 65536, 131072, len(content)]
    finally:
        await graph_http.aclose()


@pytest.mark.parametrize("failure", ["stall", "cancel"])
async def test_stream_download_stall_and_cancel_release_spool(processing, failure):
    processor, request, user, drive, service, _ = processing
    entered, closed = asyncio.Event(), asyncio.Event()
    original = drive.handle

    class Chunks(httpx.AsyncByteStream):
        async def __aiter__(self):
            entered.set()
            if failure == "stall":
                raise httpx.ReadTimeout("PRIVATE_DOWNLOAD_URL")
            await asyncio.Event().wait()
            yield b""

        async def aclose(self):
            closed.set()

    async def handle(request):
        if request.url.path.endswith("/audio/content"):
            return httpx.Response(200, stream=Chunks())
        return await original(request)

    graph_http = httpx.AsyncClient(transport=httpx.MockTransport(handle))
    factory = processor.graph_factory

    def graph(user):
        value = factory(user)
        value.http = graph_http
        return value

    processor.graph_factory = graph
    try:
        work = asyncio.create_task(processor.handle(user, request, stream=True))
        await entered.wait()
        if failure == "cancel":
            work.cancel()
            with pytest.raises(asyncio.CancelledError):
                await work
        else:
            with pytest.raises(IntelligenceError, match="temporarily_unavailable"):
                await work
        assert closed.is_set() and processor.running is None
        assert not service.calls and not drive.writes
        assert not list(Path.cwd().glob(".recording-*.wav"))
    finally:
        await graph_http.aclose()


async def test_progress_coalescing_is_bounded_and_does_not_block_producer():
    stream = ProcessingStream(None, None, None)
    for count in range(10000):
        await stream.progress("downloading", completed_bytes=count, total_bytes=10000)
    assert stream.updates.qsize() == 1
    assert (await stream.updates.get())["completed_bytes"] == 9999


def test_transport_and_timeout_defaults_preserve_http_and_voice(settings):
    assert settings.processing_deadline_seconds == 180
    assert settings.max_session_seconds == 900
    assert not settings.recording_processing_enabled
    assert processing_stream.INITIAL_REQUEST_SECONDS == 15
    assert processing_stream.MAX_MESSAGE_BYTES == 4096
    assert processing_stream.HEARTBEAT_SECONDS + processing_stream.SEND_SECONDS <= 10
    assert STREAM_READ_TIMEOUT_SECONDS == 900


class Socket:
    def __init__(self, request):
        self.incoming = asyncio.Queue()
        self.incoming.put_nowait({"type": "websocket.receive", "text": json.dumps(request)})
        self.messages = []
        self.closed = False

    async def receive(self):
        return await self.incoming.get()

    async def send_text(self, raw):
        self.messages.append(json.loads(raw))

    async def close(self, **kwargs):
        self.closed = True


def request_body(request):
    return {"v": 1, "drive_id": request.drive_id, "item_id": request.item_id,
            "source_sha1": request.source_sha1, "source_size": request.source_size}


async def test_slow_writer_cancels_owned_work(processing, monkeypatch):
    processor, request, user, drive, service, _ = processing
    service.wait = asyncio.Event()
    socket = Socket(request_body(request))
    sent = socket.send_text

    async def blocked(raw):
        await sent(raw)
        if json.loads(raw)["type"] == "progress":
            await asyncio.Event().wait()

    socket.send_text = blocked
    monkeypatch.setattr(processing_stream, "SEND_SECONDS", 0.02)
    await asyncio.wait_for(ProcessingStream(socket, processor, user).run(), 1)
    assert socket.closed and processor.running is None
    assert not drive.writes
    assert socket.messages[-1]["error"]["code"] == "temporarily_unavailable"


async def test_lost_terminal_delivery_preserves_durable_sidecars(processing):
    processor, request, user, drive, service, _ = processing
    socket = Socket(request_body(request))
    sent = socket.send_text

    async def lost(raw):
        if json.loads(raw)["type"] == "result":
            raise WebSocketDisconnect()
        await sent(raw)

    socket.send_text = lost
    await ProcessingStream(socket, processor, user).run()
    assert socket.closed and processor.running is None
    result = await processor.handle(user, request, status=True)
    assert result["status"] == "already_completed"
    assert len(drive.writes) == 2 and len(service.calls) == 1


async def test_shutdown_cancels_entire_stream_task_tree(processing):
    processor, request, user, drive, service, _ = processing
    service.wait = asyncio.Event()
    socket = Socket(request_body(request))
    baseline = asyncio.all_tasks()
    task = asyncio.create_task(ProcessingStream(socket, processor, user).run())
    while not service.calls:
        await asyncio.sleep(0)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert socket.closed and processor.running is None and not drive.writes
    assert not (asyncio.all_tasks() - baseline)


@pytest.mark.parametrize("oversize", [False, True])
async def test_real_transport_fragmented_request_total_bound(api, oversize):
    client, body, key, claims, drive, service = api
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]
    running = uvicorn.Server(uvicorn.Config(
        client.app, lifespan="off", ws="websockets", ws_max_size=4096,
        ws_max_queue=4, ws_per_message_deflate=False, access_log=False,
        log_level="critical", timeout_graceful_shutdown=2))
    task = asyncio.create_task(running.serve(sockets=[listener]))
    try:
        async with asyncio.timeout(3):
            while not running.started:
                if task.done():
                    task.result()
                await asyncio.sleep(0.01)
        async with connect(f"ws://127.0.0.1:{port}{PATH}",
                           additional_headers={"Authorization": token(key, claims)},
                           subprotocols=[SUBPROTOCOL], proxy=None) as ws:
            # Valid JSON padded to exactly the byte limit is accepted; fragmentation
            # cannot evade the limit even if individual frames are smaller.
            raw = json.dumps(body)
            raw += " " * ((4097 if oversize else 4096) - len(raw))
            await ws.send([raw[:2048], raw[2048:]])
            if oversize:
                with pytest.raises(ConnectionClosed) as exc:
                    while True:
                        await ws.recv()
                assert exc.value.rcvd.code == 1009
                assert not drive.calls and not service.calls
            else:
                async with asyncio.timeout(3):
                    while True:
                        message = json.loads(await ws.recv())
                        if message["type"] == "result":
                            assert message["status"] == "completed"
                            break
    finally:
        running.should_exit = True
        await asyncio.wait_for(task, 3)
        listener.close()
