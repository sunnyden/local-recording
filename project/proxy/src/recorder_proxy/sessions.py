import asyncio
from collections import deque
from contextlib import suppress
from dataclasses import dataclass, field
import hashlib
import logging
import time
from uuid import uuid4

import anyio
from starlette.websockets import WebSocketDisconnect

from .audio import AudioError, OutputAudio
from .context import ContextError, decode_context
from .protocol import ContextChunk, Frame, ProtocolError, Sequence, control, parse_control
from .providers import ProviderError, ResponseNotDispatched
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


class PlaybackPacer:
    def __init__(self, rate=32000, burst_samples=640):
        self.rate = rate
        self.burst_samples = burst_samples
        self.tokens = 0.0
        self.updated_at = None

    def reset(self, now):
        self.tokens = 0.0
        self.updated_at = now

    def delay(self, now, samples):
        if not 0 < samples <= self.burst_samples:
            raise ValueError("Invalid pacing request")
        if self.updated_at is None:
            self.reset(now)
        elapsed = max(0.0, now - self.updated_at)
        self.tokens = min(self.burst_samples, self.tokens + elapsed * self.rate)
        self.updated_at = now
        if self.tokens >= samples:
            self.tokens -= samples
            return 0.0
        delay = (samples - self.tokens) / self.rate
        self.tokens = 0.0
        self.updated_at = now + delay
        return delay


@dataclass
class AudioSegment:
    item_id: str
    content_index: int
    start: int | None = None
    end: int | None = None
    confirmed_done: bool = False
    audio: OutputAudio | None = None
    pending: deque[bytes] = field(default_factory=deque)


@dataclass
class Playback:
    epoch: int
    response_id: str
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
    pacer: PlaybackPacer = field(default_factory=PlaybackPacer)
    last_progress: float = field(default_factory=time.monotonic)


class VoiceSession:
    def __init__(self, socket, provider, settings, principal, *, tools=None, protocol_version=1):
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
        self.output_capacity_changed = asyncio.Event()
        self.queued_output_bytes = 0
        self.max_queued_output_bytes = self.settings.max_unplayed_samples * 2
        self.started_at = time.monotonic()
        self.ready = False
        self.tasks = []
        self.closing = False
        self.terminal_stop_sent = False
        self.transport_close_completed = False
        self.tools = VoiceTools(tools, provider, self._tool_continue) if tools else None
        self.protocol_version = protocol_version

    async def emit(self, kind, **fields):
        await self.outbound.put(control(kind, version=self.protocol_version, **fields))

    async def _receive(self):
        message = await self.socket.receive()
        if message["type"] == "websocket.disconnect":
            raise WebSocketDisconnect(message.get("code", 1000))
        return message

    async def handshake(self):
        timeout = self.settings.handshake_seconds + (30 if self.protocol_version == 2 else 0)
        async with asyncio.timeout(timeout):
            first = await self._receive()
            if first.get("text") is None:
                raise ProtocolError("Expected hello first")
            hello = parse_control(first["text"], self.protocol_version)
            if hello["type"] != "hello":
                raise ProtocolError("Expected hello first")
            encoded_context = bytearray()
            if self.protocol_version == 2:
                metadata = hello["context"]
                sequence = offset = 0
                while len(encoded_context) < metadata["length"]:
                    message = await self._receive()
                    if message.get("bytes") is None:
                        raise ProtocolError("Expected context chunk")
                    chunk = ContextChunk.parse(message["bytes"])
                    if chunk.sequence != sequence or chunk.offset != offset:
                        raise ProtocolError("Context sequence or offset mismatch")
                    if len(encoded_context) + len(chunk.data) > metadata["length"]:
                        raise ProtocolError("Context exceeds declared length")
                    encoded_context.extend(chunk.data)
                    sequence += 1
                    offset += len(chunk.data)
                if hashlib.sha256(encoded_context).hexdigest() != metadata["sha256"]:
                    raise ProtocolError("Context digest mismatch")
                try:
                    context_pcm = decode_context(bytes(encoded_context))
                except ContextError:
                    raise ProtocolError("Invalid audio context") from None
            else:
                context_pcm = b""
            await self.socket.send_json(control(
                "state", version=self.protocol_version, state="connecting",
            ))
            # Race connection setup with expiry/disconnect; no audio is accepted before ready.
            opening = asyncio.create_task(
                self.provider.open(context_pcm) if self.protocol_version == 2 else self.provider.open()
            )
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
                    if (message.get("text") is not None
                            and parse_control(message["text"], self.protocol_version)["type"] == "stop"):
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
            await self.socket.send_json(control("ready", version=self.protocol_version, session_id=self.id,
                                                max_session_seconds=self.settings.max_session_seconds))
            self.ready = True
            await self.socket.send_json(control(
                "state", version=self.protocol_version, state="listening",
            ))

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
            data = parse_control(message["text"], self.protocol_version)
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
        if self.tools:
            self.tools.retry_continuation()
        async with self.response_lock:
            if (not self.response_pending or self.speech_active or self.current_response
                    or self.response_requested
                    or (self.tools and self.tools.waiting)
                    or any(s.cleared and not s.acknowledged for s in self.streams.values())):
                return
            await self._request_response(consume_pending=True)

    async def _tool_continue(self):
        generation = self.tools.generation if self.tools else None
        async with self.response_lock:
            if (not self.tools or generation != self.tools.generation
                    or self.speech_active or self.current_response or self.response_requested
                    or self.closing
                    or any(s.cleared and not s.acknowledged for s in self.streams.values())):
                return False
            await self._request_response()
            return True

    async def _request_response(self, *, consume_pending=False):
        self.response_requested = True
        if consume_pending:
            self.response_pending = False
        try:
            await self.provider.respond()
        except ResponseNotDispatched:
            self.response_requested = False
            if consume_pending:
                self.response_pending = True
            logger.warning("voice_response_reservation_released dispatch=not_started")
            raise
        except asyncio.CancelledError:
            # Injected providers without dispatch evidence cannot safely release a reservation.
            logger.warning("voice_response_reservation_cancelled dispatch=unknown")
            raise ProviderError() from None

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

    async def _queue_audio_packet(self, stream, segment, packet):
        while self.queued_output_bytes + len(packet) > self.max_queued_output_bytes:
            if stream.cleared:
                return False
            self.output_capacity_changed.clear()
            if self.queued_output_bytes + len(packet) <= self.max_queued_output_bytes:
                continue
            await self.output_capacity_changed.wait()
        if stream.cleared:
            return False
        segment.pending.append(packet)
        self.queued_output_bytes += len(packet)
        self.audio_changed.set()
        return True

    async def _convert_audio(self, stream, segment, pcm=b"", final=False):
        packets = segment.audio.feed(pcm, final=final)
        try:
            for packet in packets:
                if not await self._queue_audio_packet(stream, segment, packet):
                    return False
        finally:
            packets.clear()
        return True

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
            segment = AudioSegment(
                event.item_id, event.content_index,
                audio=OutputAudio(self.provider.sample_rate),
            )
            stream.segments.append(segment)
        if segment.confirmed_done:
            raise ProviderError()
        if event.type == "audio_done":
            if not await self._convert_audio(stream, segment, final=True):
                return
            segment.confirmed_done = True
            segment.audio = None
        else:
            await self._convert_audio(stream, segment, event.pcm)
        self.audio_changed.set()

    async def _finish_response_audio(self, stream):
        stream.generation_done = True
        for segment in stream.segments:
            if segment.audio is not None:
                if not await self._convert_audio(stream, segment, final=True):
                    return
                segment.audio = None
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
                    if segment.start is None:
                        segment.start = stream.produced
                    while segment.pending and not stream.cleared:
                        packet = segment.pending.popleft()
                        samples = len(packet) // 2
                        split = self.settings.startup_prefill_samples - stream.produced
                        parts = (
                            (packet[:split * 2], packet[split * 2:])
                            if 0 < split < samples else (packet,)
                        )
                        for part in parts:
                            frame = Frame(
                                2, stream.epoch, stream.sequence, stream.produced, part,
                            )
                            stream.sequence += 1
                            stream.produced += len(part) // 2
                            await self.outbound.put(frame)
                    if stream.cleared or not segment.confirmed_done:
                        break
                    segment.end = stream.produced
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
                self.queued_output_bytes -= sum(map(len, segment.pending))
                segment.pending.clear()
                segment.audio = None
            if stream.started:
                clearing.append(stream.epoch)
            else:
                for segment in stream.segments:
                    await self.provider.truncate(segment.item_id, segment.content_index, 0)
                stream.acknowledged = True
        self.progress_changed.set()
        self.audio_changed.set()
        self.output_capacity_changed.set()
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
                    if self.tools and self.tools.finished_response(event.response_id):
                        logger.warning("voice_tools_duplicate_response_done_ignored")
                        continue
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
        self.queued_output_bytes -= len(frame.pcm)
        self.output_capacity_changed.set()
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
        prefill = self.settings.startup_prefill_samples
        if stream.sent >= prefill:
            delay = stream.pacer.delay(time.monotonic(), len(frame.pcm) // 2)
            if delay:
                await asyncio.sleep(delay)
        if stream.cleared:
            return
        if time.monotonic() - born > self.settings.output_queue_age_seconds:
            raise SessionError("queue_expired", True)
        if stream.sent == stream.played:
            stream.last_progress = time.monotonic()
        stream.sent += len(frame.pcm) // 2
        await self.socket.send_bytes(frame.encode())
        if stream.sent == prefill:
            stream.pacer.reset(time.monotonic())

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
        try:
            with suppress(WebSocketDisconnect, OSError, RuntimeError, TimeoutError):
                async with asyncio.timeout(1):
                    await self.socket.send_json(control(
                        "state", version=self.protocol_version, state="stopping",
                    ))
                    if error:
                        await self.socket.send_json(control(
                            "error", code=error.code, message="Voice session stopped.",
                            retryable=error.retryable, version=self.protocol_version,
                        ))
                    await self.socket.send_json(control("stop", version=self.protocol_version))
                    self.terminal_stop_sent = True
        finally:
            # A failed terminal write must not skip closing an accepted transport.
            with suppress(WebSocketDisconnect, OSError, RuntimeError, TimeoutError):
                async with asyncio.timeout(1):
                    await self.socket.close(code=1008 if error else 1000)
                    self.transport_close_completed = True

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
                    await self._finish(error)
                    logger.warning("voice_session_end code=%s stop_sent=%s close_completed=%s",
                                   error.code if error else "normal", self.terminal_stop_sent,
                                   self.transport_close_completed)
