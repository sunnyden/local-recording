import asyncio
from collections import deque
from contextlib import suppress
from dataclasses import dataclass, field
import logging
import time
from uuid import uuid4

import anyio
from starlette.websockets import WebSocketDisconnect

from .audio import AudioError, OutputAudio
from .protocol import Frame, ProtocolError, Sequence, control, parse_control
from .providers import ProviderError
from .voice_tools import VoiceTools

logger = logging.getLogger("recorder_proxy.session")


class SessionError(Exception):
    def __init__(self, code, retryable=False):
        self.code = code
        self.retryable = retryable


class Stopped(Exception):
    pass


class TimedQueue:
    def __init__(self, count, age):
        self.queue = asyncio.Queue(maxsize=count)
        self.age = age

    async def put(self, value):
        born = time.monotonic()
        try:
            async with asyncio.timeout(self.age):
                await self.queue.put((born, value))
        except TimeoutError:
            raise SessionError("backpressure", True) from None

    async def get(self):
        born, value = await self.queue.get()
        if time.monotonic() - born > self.age:
            raise SessionError("queue_expired", True)
        return born, value


@dataclass
class AudioSegment:
    item_id: str
    content_index: int
    start: int | None = None
    end: int | None = None
    confirmed_done: bool = False
    pending: deque[bytes] = field(default_factory=deque)


@dataclass
class Playback:
    epoch: int
    response_id: str
    audio: OutputAudio | None = None
    audio_item_id: str = ""
    audio_content_index: int = 0
    segments: list[AudioSegment] = field(default_factory=list)
    segment_cursor: int = 0
    generation_done: bool = False
    started: bool = False
    sequence: int = 0
    produced: int = 0
    sent: int = 0
    played: int = 0
    done: bool = False
    end_sent: bool = False
    cleared: bool = False
    clear_sent: bool = False
    acknowledged: bool = False
    clear_deadline: float = 0
    next_send_at: float = 0
    last_progress: float = field(default_factory=time.monotonic)


class VoiceSession:
    def __init__(self, socket, provider, settings, principal, *, tools=None):
        self.socket, self.provider = socket, provider
        self.settings, self.principal = settings, principal
        self.id = str(uuid4())
        self.microphone = TimedQueue(settings.queue_frames, settings.queue_age_seconds)
        # Provider bursts remain memory bounded while the writer paces the
        # device. Audio can wait longer than microphone frames without going stale.
        self.outbound = TimedQueue(settings.output_queue_frames,
                                   settings.output_queue_age_seconds)
        self.mic_sequence = Sequence()
        self.streams = {}
        self.retired = {}
        self.epoch = 0
        self.current_response = None
        self.response_pending = False
        self.response_requested = False
        self.speech_active = False
        self.response_lock = asyncio.Lock()
        self.discard_responses = {}
        self.progress_changed = asyncio.Event()
        self.audio_changed = asyncio.Event()
        self.pending_audio_bytes = 0
        self.started_at = time.monotonic()
        self.ready = False
        self.tasks = []
        self.closing = False
        self.tools = VoiceTools(tools, provider, self._tool_continue) if tools else None

    async def emit(self, kind, **fields):
        await self.outbound.put(control(kind, **fields))

    async def _receive(self):
        message = await self.socket.receive()
        if message["type"] == "websocket.disconnect":
            raise WebSocketDisconnect(message.get("code", 1000))
        return message

    async def handshake(self):
        async with asyncio.timeout(self.settings.handshake_seconds):
            first = await self._receive()
            if first.get("text") is None or parse_control(first["text"])["type"] != "hello":
                raise ProtocolError("Expected hello first")
            await self.socket.send_json(control("state", state="connecting"))
            # Race connection setup with expiry/disconnect; no audio is accepted before ready.
            opening = asyncio.create_task(self.provider.open())
            incoming = asyncio.create_task(self._receive())
            try:
                remaining = self.principal.expires_at - time.time()
                if remaining <= 0:
                    raise SessionError("token_expired")
                done, _ = await asyncio.wait(
                    (opening, incoming), timeout=min(remaining, self.settings.max_session_seconds),
                    return_when=asyncio.FIRST_COMPLETED,
                )
                if incoming in done:
                    message = incoming.result()
                    if message.get("text") is not None and parse_control(message["text"])["type"] == "stop":
                        raise Stopped()
                    raise ProtocolError("Audio/control before ready")
                if opening not in done:
                    raise SessionError("token_expired" if time.time() >= self.principal.expires_at
                                       else "session_limit")
                opening.result()
            finally:
                for task in (opening, incoming):
                    if not task.done():
                        task.cancel()
                with anyio.CancelScope(shield=True):
                    await asyncio.gather(opening, incoming, return_exceptions=True)
            if time.time() >= self.principal.expires_at:
                raise SessionError("token_expired")
            await self.socket.send_json(control("ready", session_id=self.id,
                                                max_session_seconds=self.settings.max_session_seconds))
            self.ready = True
            await self.socket.send_json(control("state", state="listening"))

    async def device_reader(self):
        while True:
            message = await self._receive()
            if message.get("bytes") is not None:
                frame = Frame.parse(message["bytes"])
                self.mic_sequence.accept(frame)
                await self.microphone.put(frame.pcm)
                continue
            if message.get("text") is None:
                raise ProtocolError("Expected audio or control")
            data = parse_control(message["text"])
            if data["type"] == "stop":
                raise Stopped()
            if data["type"] == "hello":
                raise ProtocolError("Repeated hello")
            await self.playback_report(data)

    async def playback_report(self, data):
        stream = self.streams.get(data["epoch"])
        if stream is None:
            previous = self.retired.get(data["epoch"])
            if (previous and previous[0] == data["played_samples"]
                    and (data["type"] == "playback.progress" or previous[1])):
                return
            raise ProtocolError("Unknown playback epoch")
        played = data["played_samples"]
        if not stream.played <= played <= stream.sent or stream.acknowledged:
            raise ProtocolError("Invalid played position")
        if played > stream.played:
            stream.last_progress = time.monotonic()
        stream.played = played
        self.progress_changed.set()
        self.audio_changed.set()
        if data["type"] == "playback.cleared":
            if not stream.clear_sent:
                raise ProtocolError("Unexpected clear acknowledgment")
            # Only the final actual DAC-consumed sample position is authoritative.
            if not stream.segments:
                raise ProtocolError("Playback has no provider segment")
            available = [candidate for candidate in stream.segments if candidate.start is not None]
            if not available:
                raise ProtocolError("No emitted provider segment")
            segment = available[-1]
            for candidate in available:
                boundary = candidate.end if candidate.end is not None else stream.produced
                if played <= boundary:
                    segment = candidate
                    break
            boundary = segment.end if segment.end is not None else stream.produced
            relative_played = max(0, min(played, boundary) - segment.start)
            await self.provider.truncate(
                segment.item_id, segment.content_index, relative_played,
            )
            for later in stream.segments[stream.segments.index(segment) + 1:]:
                await self.provider.truncate(later.item_id, later.content_index, 0)
            stream.acknowledged = True
            await self._maybe_respond()

    async def _maybe_respond(self):
        async with self.response_lock:
            if (not self.response_pending or self.speech_active or self.current_response
                    or self.response_requested
                    or (self.tools and self.tools.waiting)
                    or any(s.cleared and not s.acknowledged for s in self.streams.values())):
                return
            self.response_pending = False
            self.response_requested = True
            await self.provider.respond()

    async def _tool_continue(self):
        async with self.response_lock:
            if self.speech_active or self.current_response or self.response_requested or self.closing:
                return
            self.response_requested = True
            await self.provider.respond()

    async def microphone_writer(self):
        while True:
            _, pcm = await self.microphone.get()
            await self.provider.append(pcm)

    def _retire_streams(self):
        for epoch, stream in list(self.streams.items()):
            if stream.acknowledged or (stream.end_sent and stream.played == stream.produced and not stream.cleared):
                self.retired[epoch] = (stream.played, stream.cleared)
                del self.streams[epoch]
        while len(self.retired) > 4:
            del self.retired[next(iter(self.retired))]

    async def _stream(self, event):
        for stream in self.streams.values():
            if stream.response_id == event.response_id:
                return stream
        self._retire_streams()
        if len(self.streams) >= 4:
            raise SessionError("playback_backlog", True)
        self.epoch += 1
        stream = Playback(self.epoch, event.response_id)
        self.streams[stream.epoch] = stream
        return stream

    async def _emit_audio(self, stream, pcm, final=False):
        for packet in stream.audio.feed(pcm, final=final):
            if stream.cleared:
                return
            frame = Frame(2, stream.epoch, stream.sequence, stream.produced, packet)
            stream.sequence += 1
            stream.produced += len(packet) // 2
            await self.outbound.put(frame)

    async def _finish_active_item(self, stream):
        if stream.audio is None:
            return
        await self._emit_audio(stream, b"", final=True)
        segment = stream.segments[stream.segment_cursor]
        segment.end = stream.produced
        stream.audio = None
        stream.audio_item_id = ""

    async def _audio(self, stream, event):
        if stream.generation_done:
            raise ProviderError()
        key = (event.item_id, event.content_index)
        segment = next((
            value for value in stream.segments
            if (value.item_id, value.content_index) == key
        ), None)
        if segment is None:
            if len(stream.segments) >= 128:
                raise SessionError("provider_item_limit")
            segment = AudioSegment(event.item_id, event.content_index)
            stream.segments.append(segment)
        if segment.confirmed_done:
            raise ProviderError()
        if event.type == "audio_done":
            segment.confirmed_done = True
        else:
            if self.pending_audio_bytes + len(event.pcm) > 2 * 1024 * 1024:
                raise SessionError("provider_audio_limit", True)
            segment.pending.append(event.pcm)
            self.pending_audio_bytes += len(event.pcm)
        self.audio_changed.set()

    async def _finish_response_audio(self, stream):
        stream.generation_done = True
        for segment in stream.segments:
            segment.confirmed_done = True
        self.audio_changed.set()

    async def audio_writer(self):
        """Drain ordered items independently so delayed done never blocks ingress."""
        while True:
            self.audio_changed.clear()
            for stream in list(self.streams.values()):
                if stream.cleared or stream.done:
                    continue
                if any(not previous.cleared and
                       (not previous.end_sent or previous.played != previous.produced)
                       for previous in self.streams.values() if previous.epoch < stream.epoch):
                    continue
                if not stream.started:
                    stream.segments[0].start = stream.produced
                    stream.started = True
                    await self.emit("playback.start", epoch=stream.epoch)
                    await self.emit("state", state="speaking")
                while stream.segment_cursor < len(stream.segments) and not stream.cleared:
                    segment = stream.segments[stream.segment_cursor]
                    if stream.audio is None:
                        segment.start = stream.produced
                        stream.audio = OutputAudio(self.provider.sample_rate)
                        stream.audio_item_id = segment.item_id
                        stream.audio_content_index = segment.content_index
                    while segment.pending and not stream.cleared:
                        chunk = segment.pending.popleft()
                        self.pending_audio_bytes -= len(chunk)
                        await self._emit_audio(stream, chunk)
                    if stream.cleared or not segment.confirmed_done:
                        break
                    await self._finish_active_item(stream)
                    stream.segment_cursor += 1
                if (not stream.cleared and stream.generation_done and
                        stream.segment_cursor == len(stream.segments)):
                    stream.done = True
                    await self.emit("playback.end", epoch=stream.epoch)
            await self.audio_changed.wait()

    async def interrupt(self):
        if self.tools:
            self.tools.cancel()
        self._retire_streams()
        if self.current_response:
            self.discard_responses.setdefault(self.current_response, None)
        clearing = []
        for stream in self.streams.values():
            if stream.cleared or stream.acknowledged:
                continue
            self.discard_responses.setdefault(stream.response_id, None)
            stream.cleared = True
            for segment in stream.segments:
                self.pending_audio_bytes -= sum(map(len, segment.pending))
                segment.pending.clear()
            if stream.started:
                clearing.append(stream.epoch)
            else:
                for segment in stream.segments:
                    await self.provider.truncate(segment.item_id, segment.content_index, 0)
                stream.acknowledged = True
        self.progress_changed.set()
        self.audio_changed.set()
        for epoch in clearing:
            # Writer discards obsolete queued audio. It alone sends clear, preserving wire order.
            await self.emit("playback.clear", epoch=epoch)
        while len(self.discard_responses) > 32:
            del self.discard_responses[next(iter(self.discard_responses))]
        await self.emit("state", state="listening")

    async def provider_reader(self):
        async for event in self.provider.events():
            if event.type == "speech_started":
                self.speech_active = True
                await self.interrupt()
            elif event.type == "speech_stopped":
                self.speech_active = False
                await self.emit("state", state="listening")
            elif event.type == "input_committed":
                if self.tools:
                    self.tools.cancel(new_turn=True)
                self.response_pending = True
                await self._maybe_respond()
            elif event.type == "response_started":
                if self.current_response is not None or not self.response_requested:
                    raise ProviderError()
                if self.speech_active:
                    # Don't emit a response created during an in-flight VAD/request race.
                    raise SessionError("interruption_race", True)
                self.response_requested = False
                self.current_response = event.response_id
                if self.tools:
                    self.tools.start(event.response_id)
            elif event.type in ("tool_added", "tool_delta", "tool_done"):
                if event.response_id in self.discard_responses:
                    continue
                if not self.tools:
                    raise ProviderError()
                self.tools.accept(event)
            elif event.type in ("audio", "audio_done"):
                if event.response_id in self.discard_responses:
                    continue
                if event.response_id != self.current_response:
                    raise ProviderError()
                stream = await self._stream(event)
                if stream.cleared:
                    continue
                await self._audio(stream, event)
            elif event.type == "response_done":
                if event.response_id != self.current_response:
                    raise ProviderError()
                for stream in list(self.streams.values()):
                    if stream.response_id == event.response_id and not stream.cleared:
                        await self._finish_response_audio(stream)
                self.current_response = None
                if self.tools:
                    self.tools.response_done(event.response_id)
                # Recent canceled IDs discard late audio; older unknown IDs fail closed.
                await self.emit("state", state="listening")
                await self._maybe_respond()
            else:
                raise ProviderError()
        raise ProviderError()

    async def _send_frame(self, born, frame):
        stream = self.streams.get(frame.epoch)
        if not stream or stream.cleared:
            return
        while (sum(s.sent - s.played for s in self.streams.values() if not s.cleared)
               + len(frame.pcm) // 2 > self.settings.max_unplayed_samples):
            self.progress_changed.clear()
            remaining = self.settings.output_queue_age_seconds - (time.monotonic() - born)
            if remaining <= 0:
                raise SessionError("playback_credit_timeout", True)
            try:
                async with asyncio.timeout(remaining):
                    await self.progress_changed.wait()
            except TimeoutError:
                raise SessionError("playback_credit_timeout", True) from None
            if stream.cleared:
                return
        # Fill the board's complete bounded credit before real-time pacing.
        # This covers its four-period DMA priming latency and network jitter.
        now = time.monotonic()
        if stream.sent < self.settings.startup_prefill_samples:
            send_at = now
        else:
            send_at = max(stream.next_send_at, now - 0.04)
            if send_at > now:
                await asyncio.sleep(send_at - now)
            send_at = max(send_at, time.monotonic() - 0.04)
        if stream.cleared:
            return
        if time.monotonic() - born > self.settings.output_queue_age_seconds:
            raise SessionError("queue_expired", True)
        stream.next_send_at = send_at + len(frame.pcm) / 32000
        if stream.sent == stream.played:
            stream.last_progress = time.monotonic()
        stream.sent += len(frame.pcm) // 2
        await self.socket.send_bytes(frame.encode())

    async def device_writer(self):
        while True:
            born, value = await self.outbound.get()
            async with asyncio.timeout(self.settings.output_queue_age_seconds):
                if isinstance(value, Frame):
                    await self._send_frame(born, value)
                    continue
                stream = self.streams.get(value.get("epoch"))
                if value["type"] == "playback.end" and (not stream or stream.cleared):
                    continue
                # A start can precede a clear even if interruption happened before its first packet.
                if value["type"] == "playback.clear":
                    stream.clear_sent = True
                    stream.clear_deadline = time.monotonic() + self.settings.clear_timeout_seconds
                elif value["type"] == "playback.end":
                    stream.end_sent = True
                    self.progress_changed.set()
                    self.audio_changed.set()
                await self.socket.send_json(value)

    async def watchdog(self):
        while True:
            now = time.monotonic()
            if time.time() >= self.principal.expires_at:
                raise SessionError("token_expired")
            if now - self.started_at >= self.settings.max_session_seconds:
                raise SessionError("session_limit")
            for stream in self.streams.values():
                if stream.clear_sent and not stream.acknowledged and now > stream.clear_deadline:
                    raise SessionError("clear_timeout", True)
                if not stream.cleared and stream.sent > stream.played:
                    if now - stream.last_progress > self.settings.playback_stall_seconds:
                        raise SessionError("playback_stalled", True)
            await asyncio.sleep(0.05)

    async def _finish(self, error=None):
        if self.closing:
            return
        self.closing = True
        with suppress(WebSocketDisconnect, OSError, RuntimeError, TimeoutError):
            async with asyncio.timeout(1):
                await self.socket.send_json(control("state", state="stopping"))
                if error:
                    await self.socket.send_json(control(
                        "error", code=error.code, message="Voice session stopped.",
                        retryable=error.retryable,
                    ))
                await self.socket.send_json(control("stop"))
                await self.socket.close(code=1008 if error else 1000)

    async def run(self):
        error = None
        try:
            await self.handshake()
            self.tasks = [asyncio.create_task(coro()) for coro in (
                self.device_reader, self.microphone_writer, self.provider_reader,
                self.audio_writer, self.device_writer, self.watchdog,
            )]
            if self.tools:
                self.tasks.append(asyncio.create_task(self.tools.watch()))
            done, _ = await asyncio.wait(self.tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except (Stopped, WebSocketDisconnect):
            pass
        except ProtocolError:
            error = SessionError("protocol_error")
        except (ProviderError, AudioError):
            error = SessionError("provider_unavailable", True)
        except SessionError as failure:
            error = failure
        except TimeoutError:
            error = SessionError("timeout", True)
        except OSError:
            error = SessionError("connection_error", True)
        finally:
            # ASGI servers/test clients can use level-triggered AnyIO cancellation.
            # Cleanup must not be canceled again at each await within that scope.
            with anyio.CancelScope(shield=True):
                for task in self.tasks:
                    task.cancel()
                await asyncio.gather(*self.tasks, return_exceptions=True)
                if self.tools:
                    await self.tools.close()
                try:
                    await self.provider.close()
                except ProviderError:
                    error = error or SessionError("provider_unavailable", True)
                finally:
                    logger.warning("voice_session_end code=%s",
                                   error.code if error else "normal")
                    await self._finish(error)
