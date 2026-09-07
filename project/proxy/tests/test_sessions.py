import asyncio
from dataclasses import replace
import json
import time

import pytest

from recorder_proxy.auth import Principal
from recorder_proxy.protocol import Frame, ProtocolError, control
from recorder_proxy.providers import Event, ProviderError
from recorder_proxy.sessions import SessionError, TimedQueue, VoiceSession


HELLO = control("hello", sample_rate=16000, channels=1, format="pcm16", frame_samples=320)


class FakeSocket:
    def __init__(self):
        self.incoming = asyncio.Queue()
        self.outgoing = asyncio.Queue()
        self.history = []
        self.closed = False

    async def receive(self):
        return await self.incoming.get()

    async def send_json(self, data):
        self.history.append(data)
        await self.outgoing.put(data)

    async def send_bytes(self, data):
        self.history.append(data)
        await self.outgoing.put(data)

    async def close(self, code=1000):
        self.closed = True

    async def send(self, data):
        message = ({"bytes": data} if isinstance(data, bytes) else {"text": json.dumps(data)})
        await self.incoming.put({"type": "websocket.receive", **message})

    async def until(self, kind):
        async with asyncio.timeout(3):
            while True:
                data = await self.outgoing.get()
                if kind == "audio" and isinstance(data, bytes):
                    return Frame.parse(data, 2)
                if isinstance(data, dict) and data.get("type") == kind:
                    return data


class FakeProvider:
    sample_rate = 16000

    def __init__(self):
        self.messages = asyncio.Queue()
        self.opened = asyncio.Event()
        self.accepted = asyncio.Event()
        self.accepted.set()
        self.closed = False
        self.appended = []
        self.truncations = []
        self.responses = 0
        self.response_requests = asyncio.Queue()
        self.open_error = False
        self.append_block = None

    async def open(self):
        self.opened.set()
        await self.accepted.wait()
        if self.open_error:
            raise ProviderError("DO NOT LEAK")

    async def append(self, pcm):
        if self.append_block:
            await self.append_block.wait()
        self.appended.append(pcm)

    async def events(self):
        while True:
            event = await self.messages.get()
            if isinstance(event, Exception):
                raise event
            yield event

    async def truncate(self, item_id, content_index, played_samples):
        self.truncations.append((item_id, content_index, played_samples))

    async def respond(self):
        self.responses += 1
        self.response_requests.put_nowait(self.responses)

    async def close(self):
        self.closed = True


async def start(settings, principal, provider=None):
    socket, provider = FakeSocket(), provider or FakeProvider()
    session = VoiceSession(socket, provider, settings, principal)
    await socket.send(HELLO)
    task = asyncio.create_task(session.run())
    await socket.until("ready")
    return socket, provider, session, task


async def stop(socket, provider, session, task):
    await socket.send(control("stop"))
    await asyncio.wait_for(task, 3)
    assert socket.closed and provider.closed
    assert all(t.done() for t in session.tasks)


async def test_ready_only_after_configuration(settings, principal):
    socket, provider = FakeSocket(), FakeProvider()
    provider.accepted.clear()
    session = VoiceSession(socket, provider, settings, principal)
    await socket.send(HELLO)
    task = asyncio.create_task(session.run())
    await provider.opened.wait()
    assert not session.ready
    assert not any(d.get("type") == "ready" for d in socket.history)
    provider.accepted.set()
    await socket.until("ready")
    await stop(socket, provider, session, task)


async def test_full_duplex_and_progress_truncation(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    await provider.messages.put(Event("response_started", "r1"))
    await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(640)))
    frame = await socket.until("audio")
    assert (frame.epoch, frame.sequence, frame.position) == (1, 0, 0)
    await socket.send(Frame(1, 0, 0, 0, bytes(640)).encode())
    await socket.send(control("playback.progress", epoch=1, played_samples=100))
    await provider.messages.put(Event("speech_started"))
    assert (await socket.until("playback.clear"))["epoch"] == 1
    await socket.send(control("playback.cleared", epoch=1, played_samples=123))
    await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(640)))
    await provider.messages.put(Event("response_done", "r1"))
    await provider.messages.put(Event("speech_stopped"))
    await provider.messages.put(Event("input_committed"))
    assert await provider.response_requests.get() == 1
    assert await asyncio.wait_for(provider.response_requests.get(), 2) == 2
    await provider.messages.put(Event("response_started", "r2"))
    await provider.messages.put(Event("audio", "r2", "i2", 0, bytes(640)))
    new = await socket.until("audio")
    assert (new.epoch, new.sequence, new.position) == (2, 0, 0)
    assert provider.truncations == [("i1", 0, 123)]
    assert provider.appended == [bytes(640)]
    await stop(socket, provider, session, task)


async def test_response_done_flushes_short_output(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    await provider.messages.put(Event("response_started", "r1"))
    await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(200)))
    await provider.messages.put(Event("response_done", "r1"))
    assert len((await socket.until("audio")).pcm) == 200
    assert (await socket.until("playback.end"))["epoch"] == 1
    await socket.send(control("playback.progress", epoch=1, played_samples=100))
    await stop(socket, provider, session, task)


@pytest.mark.parametrize("message", [
    control("hello", sample_rate=8000, channels=1, format="pcm16", frame_samples=320),
    Frame(1, 0, 0, 0, bytes(640)).encode(),
    control("playback.progress", epoch=1, played_samples=0),
])
async def test_hello_required_before_billable_connection(settings, principal, message):
    socket, provider = FakeSocket(), FakeProvider()
    await socket.send(message)
    await VoiceSession(socket, provider, settings, principal).run()
    assert not provider.opened.is_set()
    assert provider.closed
    assert (await socket.until("error"))["code"] == "protocol_error"


@pytest.mark.parametrize("phase", ["handshake", "live"])
async def test_disconnect_cleanup(settings, principal, phase):
    provider = FakeProvider()
    if phase == "live":
        socket, provider, session, task = await start(settings, principal, provider)
    else:
        socket = FakeSocket()
        provider.accepted.clear()
        session = VoiceSession(socket, provider, settings, principal)
        await socket.send(HELLO)
        task = asyncio.create_task(session.run())
        await provider.opened.wait()
    await socket.incoming.put({"type": "websocket.disconnect", "code": 1006})
    await asyncio.wait_for(task, 3)
    assert provider.closed and socket.closed
    assert all(t.done() for t in session.tasks)


@pytest.mark.parametrize("phase", ["handshake", "live"])
async def test_external_cancellation_cleanup(settings, principal, phase):
    if phase == "live":
        socket, provider, session, task = await start(settings, principal)
    else:
        socket, provider = FakeSocket(), FakeProvider()
        provider.accepted.clear()
        session = VoiceSession(socket, provider, settings, principal)
        await socket.send(HELLO)
        task = asyncio.create_task(session.run())
        await provider.opened.wait()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert provider.closed and socket.closed
    assert all(t.done() for t in session.tasks)


async def test_live_token_expiry(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    session.principal = Principal(principal.oid, int(time.time()) - 1)
    await asyncio.wait_for(task, 3)
    assert (await socket.until("error"))["code"] == "token_expired"
    assert provider.closed


async def test_session_limit(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    session.started_at -= settings.max_session_seconds + 1
    await asyncio.wait_for(task, 3)
    assert (await socket.until("error"))["code"] == "session_limit"


async def test_clear_ack_timeout(settings, principal):
    settings = replace(settings, clear_timeout_seconds=0.02)
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    await provider.messages.put(Event("response_started", "r1"))
    await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(640)))
    await socket.until("audio")
    await provider.messages.put(Event("speech_started"))
    await asyncio.wait_for(task, 3)
    assert (await socket.until("error"))["code"] == "clear_timeout"


async def test_provider_error_redacted_and_cleaned(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(ProviderError("Authorization: secret-and-private-audio"))
    await asyncio.wait_for(task, 3)
    assert (await socket.until("error")) == {
        "v": 1, "type": "error", "code": "provider_unavailable",
        "message": "Voice session stopped.", "retryable": True,
    }
    assert "secret" not in str(socket.history)
    assert provider.closed


async def test_configuration_failure_no_ready(settings, principal):
    socket, provider = FakeSocket(), FakeProvider()
    provider.open_error = True
    await socket.send(HELLO)
    await VoiceSession(socket, provider, settings, principal).run()
    assert all(data["type"] != "ready" for data in socket.history)
    assert provider.closed


async def test_queue_overflow_and_age():
    queue = TimedQueue(1, 0.01)
    await queue.put(b"a")
    with pytest.raises(SessionError) as exc:
        await queue.put(b"b")
    assert exc.value.code == "backpressure"
    with pytest.raises(SessionError) as exc:
        await queue.get()
    assert exc.value.code == "queue_expired"


async def test_microphone_backpressure(settings, principal):
    settings = replace(settings, queue_frames=1, queue_age_seconds=0.02)
    provider = FakeProvider()
    provider.append_block = asyncio.Event()
    socket, provider, session, task = await start(settings, principal, provider)
    for i in range(8):
        await socket.send(Frame(1, 0, i, 320 * i, bytes(640)).encode())
    await asyncio.wait_for(task, 3)
    assert (await socket.until("error"))["code"] in ("backpressure", "queue_expired")
    assert provider.closed


async def test_invalid_actual_played_position(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    await provider.messages.put(Event("response_started", "r1"))
    await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(640)))
    await socket.until("audio")
    await socket.send(control("playback.progress", epoch=1, played_samples=321))
    await asyncio.wait_for(task, 3)
    assert (await socket.until("error"))["code"] == "protocol_error"


async def test_playback_backlog_is_bounded(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    await provider.messages.put(Event("response_started", "r1"))
    await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(32000)))
    await asyncio.wait_for(task, 3)
    sent = [Frame.parse(data, 2) for data in socket.history if isinstance(data, bytes)]
    assert sum(len(f.pcm) // 2 for f in sent) <= settings.max_unplayed_samples == 16000
    assert (await socket.until("error"))["code"] in (
        "playback_credit_timeout", "playback_stalled", "timeout", "queue_expired",
    )
    assert provider.closed


async def test_response_creation_waits_for_final_clear_and_truncate(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    assert await asyncio.wait_for(provider.response_requests.get(), 2) == 1
    await provider.messages.put(Event("response_started", "r1"))
    await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(640)))
    await socket.until("audio")
    await provider.messages.put(Event("speech_started"))
    await socket.until("playback.clear")
    await provider.messages.put(Event("response_done", "r1"))
    await provider.messages.put(Event("speech_stopped"))
    await provider.messages.put(Event("input_committed"))
    await asyncio.sleep(0.02)
    assert provider.responses == 1
    assert provider.truncations == []
    await socket.send(control("playback.cleared", epoch=1, played_samples=160))
    assert await asyncio.wait_for(provider.response_requests.get(), 2) == 2
    assert provider.truncations == [("i1", 0, 160)]
    await stop(socket, provider, session, task)


async def test_multiple_audio_items_in_one_response_share_device_epoch(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    await provider.messages.put(Event("response_started", "r1"))
    await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(640)))
    await provider.messages.put(Event("audio", "r1", "i2", 0, bytes(640)))
    # Voice Live can delay all item done events until after later item audio.
    await provider.messages.put(Event("audio_done", "r1", "i1"))
    await provider.messages.put(Event("audio_done", "r1", "i2"))
    await provider.messages.put(Event("response_done", "r1"))
    first = await socket.until("audio")
    second = await socket.until("audio")
    assert (first.epoch, first.sequence, first.position) == (1, 0, 0)
    assert (second.epoch, second.sequence, second.position) == (1, 1, 320)
    assert (await socket.until("playback.end"))["epoch"] == 1
    starts = [d for d in socket.history if isinstance(d, dict) and d["type"] == "playback.start"]
    assert len(starts) == 1
    await socket.send(control("playback.progress", epoch=1, played_samples=640))
    await stop(socket, provider, session, task)


async def test_multi_item_clear_maps_played_position_to_provider_item(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    await provider.messages.put(Event("response_started", "r1"))
    for item in ("i1", "i2"):
        await provider.messages.put(Event("audio", "r1", item, 0, bytes(640)))
    await provider.messages.put(Event("audio_done", "r1", "i1"))
    await provider.messages.put(Event("audio_done", "r1", "i2"))
    await socket.until("audio")
    await socket.until("audio")
    await provider.messages.put(Event("speech_started"))
    await socket.until("playback.clear")
    await socket.send(control("playback.cleared", epoch=1, played_samples=400))
    async with asyncio.timeout(1):
        while not provider.truncations:
            await asyncio.sleep(0)
    assert provider.truncations == [("i2", 0, 80)]
    await stop(socket, provider, session, task)


async def test_weather_shape_many_items_then_delayed_done(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    await provider.messages.put(Event("response_started", "weather"))
    items = [f"item-{index}" for index in range(12)]
    for item in items:
        await provider.messages.put(Event("audio", "weather", item, 0, bytes(640)))
    for item in items:
        await provider.messages.put(Event("audio_done", "weather", item))
    await provider.messages.put(Event("response_done", "weather"))
    frames = [await socket.until("audio") for _ in items]
    assert {frame.epoch for frame in frames} == {1}
    assert [frame.sequence for frame in frames] == list(range(12))
    assert (await socket.until("playback.end"))["epoch"] == 1
    await socket.send(control("playback.progress", epoch=1, played_samples=3840))
    assert not task.done()
    await stop(socket, provider, session, task)


async def test_fast_provider_with_paced_speaker_and_100ms_reports(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    await provider.messages.put(Event("input_committed"))
    await provider.messages.put(Event("response_started", "r1"))
    await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(64000)))
    await provider.messages.put(Event("response_done", "r1"))
    played = 0
    deadline = time.monotonic()
    try:
        async with asyncio.timeout(4):
            while played < 32000:
                message = await socket.outgoing.get()
                if isinstance(message, dict):
                    assert message["type"] != "error", message
                    continue
                frame = Frame.parse(message, 2)
                assert (session.streams[frame.epoch].sent - session.streams[frame.epoch].played
                        <= settings.max_unplayed_samples == 16000)
                samples = len(frame.pcm) // 2
                deadline += samples / 16000
                await asyncio.sleep(max(0, deadline - time.monotonic()))
                played += samples
                if played % 1600 == 0 or played == 32000:
                    await socket.send(control("playback.progress", epoch=frame.epoch,
                                              played_samples=played))
        assert not task.done()
        await stop(socket, provider, session, task)
    finally:
        if not task.done():
            task.cancel()
        await asyncio.gather(task, return_exceptions=True)


async def test_output_prefills_credit_then_paces(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    timestamps = []
    original_send = socket.send_bytes

    async def track_send(data):
        timestamps.append(time.monotonic())
        await original_send(data)

    socket.send_bytes = track_send
    try:
        await provider.messages.put(Event("input_committed"))
        await provider.messages.put(Event("response_started", "r1"))
        await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(640 * 30)))
        for _ in range(25):
            await socket.until("audio")
        assert timestamps[24] - timestamps[0] < 0.1
        assert session.streams[1].sent == settings.startup_prefill_samples
        await socket.send(control("playback.progress", epoch=1, played_samples=1600))
        for _ in range(5):
            await socket.until("audio")
        assert timestamps[29] - timestamps[25] >= 0.03
        assert session.streams[1].sent == 9600
        await stop(socket, provider, session, task)
    finally:
        if not task.done():
            task.cancel()
        await asyncio.gather(task, return_exceptions=True)


async def test_clear_during_pacing_discards_waiting_packet(settings, principal):
    socket, provider, session, task = await start(settings, principal)
    try:
        await provider.messages.put(Event("input_committed"))
        await provider.messages.put(Event("response_started", "r1"))
        await provider.messages.put(Event("audio", "r1", "i1", 0, bytes(640 * 4)))
        for _ in range(3):
            await socket.until("audio")
        await provider.messages.put(Event("speech_started"))
        await socket.until("playback.clear")
        assert len([message for message in socket.history if isinstance(message, bytes)]) == 4
        await stop(socket, provider, session, task)
    finally:
        if not task.done():
            task.cancel()
        await asyncio.gather(task, return_exceptions=True)
