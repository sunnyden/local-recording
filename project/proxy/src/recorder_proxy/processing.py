import asyncio
import hashlib
import logging
import os
from pathlib import Path
import struct
import time
from uuid import uuid4

from .intelligence_errors import IntelligenceError, unavailable
from .cleanup import finish_task
from .recording_contract import MAX_WAV_BYTES, verify_source
from .sidecars import MAX_SIDECAR, json_bytes, reconcile, recover_text, text_bytes
from .speech import disk

logger = logging.getLogger("recorder_proxy.processing")


def validate_wav(file):
    file.seek(0)
    header = file.read(12)
    if len(header) != 12:
        raise IntelligenceError("unsupported_audio", 415)
    riff, length, kind = struct.unpack("<4sI4s", header)
    size = os.fstat(file.fileno()).st_size
    if riff != b"RIFF" or kind != b"WAVE" or length + 8 != size:
        raise IntelligenceError("unsupported_audio", 415)
    fmt, frames, chunks = False, None, 0
    while file.tell() < size:
        chunk = file.read(8)
        chunks += 1
        if len(chunk) != 8 or chunks > 256:
            raise IntelligenceError("unsupported_audio", 415)
        name, count = struct.unpack("<4sI", chunk)
        end = file.tell() + count + (count & 1)
        if end > size:
            raise IntelligenceError("unsupported_audio", 415)
        if name == b"fmt ":
            if fmt or count not in (16, 18):
                raise IntelligenceError("unsupported_audio", 415)
            data = file.read(count)
            if (struct.unpack("<HHIIHH", data[:16]) != (1, 1, 16000, 32000, 2, 16)
                    or (count == 18 and data[16:] != b"\0\0")):
                raise IntelligenceError("unsupported_audio", 415)
            fmt = True
        elif name == b"data":
            if not fmt or frames is not None or count % 2:
                raise IntelligenceError("unsupported_audio", 415)
            frames = count // 2
        file.seek(end)
    if not fmt or frames is None or frames == 0:
        raise IntelligenceError("unsupported_audio", 415)
    if frames > 1800 * 16000:
        raise IntelligenceError("recording_too_long", 413)
    return frames / 16000


class RecordingProcessor:
    """One in-flight synchronous operation; contention is rejected, never queued."""

    def __init__(self, settings, graph_factory, speech):
        self.settings, self.graph_factory, self.speech = settings, graph_factory, speech
        self.running = None

    @staticmethod
    def result(operation, status, json_item=None, text_item=None):
        result = {"v": 1, "operation_id": operation, "status": status}
        if json_item and text_item:
            if any(len(item["id"]) > 128 for item in (json_item, text_item)):
                raise unavailable()
            result.update(json_item_id=json_item["id"], text_item_id=text_item["id"])
        return result

    async def handle(self, user, request, *, status=False, stream=False, progress=None):
        operation = request.operation(user.principal.oid)
        if self.running == operation:
            if status:
                return self.result(operation, "processing")
            raise IntelligenceError("processing_in_progress", 409, True)
        if not status:
            if self.running:
                raise IntelligenceError("busy", 429, True)
            self.running = operation
        graph = None
        phase, phase_started = None, time.monotonic()

        async def report(name, **counts):
            nonlocal phase, phase_started
            if name != phase:
                now = time.monotonic()
                if phase:
                    logger.info("recording_phase_finished phase=%s duration_ms=%d",
                                phase, (now - phase_started) * 1000)
                phase, phase_started = name, now
            if progress:
                await progress(name, **counts)

        try:
            graph = self.graph_factory(user)
            deadline = 30 if status else None if stream else self.settings.processing_deadline_seconds
            async with asyncio.timeout(deadline):
                await report("resolving")
                source = await graph.item(request.drive_id, request.item_id)
                verify_source(source, request)
                names, complete, partial, raw_text = await reconcile(graph, request, source, operation)
                if complete:
                    return self.result(operation, "already_completed", complete, partial)
                if status:
                    return self.result(operation, "retry_required" if partial else "not_started")
                return await self._process(graph, request, source, operation, names, partial,
                                           raw_text, report, stream)
        except TimeoutError:
            if stream and not status:
                raise unavailable() from None
            raise IntelligenceError("processing_deadline", 504, True) from None
        except OSError:
            raise unavailable() from None
        finally:
            if phase:
                logger.info("recording_phase_finished phase=%s duration_ms=%d",
                            phase, (time.monotonic() - phase_started) * 1000)
            try:
                if graph:
                    await finish_task(asyncio.create_task(graph.close()))
            finally:
                if not status:
                    self.running = None

    async def _unchanged(self, graph, request, source):
        current = await graph.item(request.drive_id, request.item_id)
        verify_source(current, request)
        if (current["eTag"] != source["eTag"]
                or current["parentReference"]["id"] != source["parentReference"]["id"]):
            raise IntelligenceError("source_changed", 409)

    async def _process(self, graph, request, source, operation, names, partial, raw_text,
                       report, stream):
        # The random spool file is private, relative to the application cwd, and
        # unlinked on cancellation. Neither user tokens nor transcripts are spooled.
        path = Path(f".recording-{uuid4().hex}.wav")
        file = None
        owned = False
        try:
            def open_file():
                nonlocal file, owned
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR |
                             getattr(os, "O_BINARY", 0), 0o600)
                owned = True
                file = os.fdopen(fd, "w+b")

            await disk(open_file)
            digest = hashlib.sha1()
            downloaded = 0
            await report("downloading", completed_bytes=0, total_bytes=source["size"])

            async def sink(chunk):
                nonlocal downloaded
                await disk(file.write, chunk)
                digest.update(chunk)
                downloaded += len(chunk)
                await report("downloading", completed_bytes=downloaded, total_bytes=source["size"])

            await graph.download(request.drive_id, request.item_id, MAX_WAV_BYTES, sink)
            await report("validating")
            await disk(file.flush)
            if digest.hexdigest() != request.source_sha1:
                raise IntelligenceError("source_changed", 409)
            await disk(validate_wav, file)
            await self._unchanged(graph, request, source)
            if partial:
                phrases = recover_text(raw_text, operation, request)
            else:
                await report("transcribing")
                if stream:
                    phrases = await self.speech.transcribe(file, stream=True, progress=report)
                else:
                    phrases = await self.speech.transcribe(file)
                raw_text = text_bytes(operation, request, phrases)
                if len(raw_text) > MAX_SIDECAR:
                    raise unavailable()
            await report("saving")
            await self._unchanged(graph, request, source)
            parent = source["parentReference"]["id"]
            if not partial:
                partial = await graph.put_new(request.drive_id, parent, names[1], raw_text,
                                              "text/plain; charset=utf-8")
            await self._unchanged(graph, request, source)
            document = json_bytes(operation, request, source, phrases, partial, raw_text)
            if len(document) > MAX_SIDECAR:
                raise unavailable()
            await graph.put_new(request.drive_id, parent, names[0], document,
                                "application/json; charset=utf-8")
            # JSON is written last. Read both outputs back before claiming success.
            await report("verifying")
            await self._unchanged(graph, request, source)
            _, complete, text_item, _ = await reconcile(graph, request, source, operation)
            if not complete or not text_item:
                raise unavailable()
            return self.result(operation, "completed", complete, text_item)
        finally:
            async def cleanup():
                try:
                    if file is not None:
                        await disk(file.close)
                finally:
                    if owned:
                        await disk(path.unlink, True)

            await finish_task(asyncio.create_task(cleanup()))
