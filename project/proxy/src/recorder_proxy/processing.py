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
from .recording_contract import MAX_OPUS_BYTES, verify_source
from .sidecars import MAX_SIDECAR, json_bytes, reconcile, recover_text, text_bytes
from .speech import disk

logger = logging.getLogger("recorder_proxy.processing")


def _make_ogg_crc_table():
    values = []
    for byte in range(256):
        crc = byte << 24
        for _ in range(8):
            crc = ((crc << 1) & 0xffffffff) ^ (0x04c11db7 if crc & 0x80000000 else 0)
        values.append(crc)
    return tuple(values)


_OGG_CRC_TABLE = _make_ogg_crc_table()


def _ogg_crc(data):
    crc = 0
    for byte in data:
        crc = ((crc << 8) & 0xffffffff) ^ _OGG_CRC_TABLE[((crc >> 24) ^ byte) & 0xff]
    return crc


def _opus_packet_samples(packet):
    if not packet:
        raise IntelligenceError("unsupported_audio", 415)
    config, code = packet[0] >> 3, packet[0] & 3
    if config < 12:
        frame_samples = (480, 960, 1920, 2880)[config & 3]
    elif config < 16:
        frame_samples = (480, 960)[config & 1]
    else:
        frame_samples = (120, 240, 480, 960)[config & 3]
    if code == 0:
        frames = 1
    elif code in (1, 2):
        frames = 2
    elif len(packet) >= 2:
        frames = packet[1] & 0x3f
    else:
        frames = 0
    samples = frame_samples * frames
    if not frames or samples != 960:
        raise IntelligenceError("unsupported_audio", 415)
    return samples


def _validate_opus_tags(packet):
    if len(packet) < 16 or not packet.startswith(b"OpusTags"):
        raise IntelligenceError("unsupported_audio", 415)
    vendor = struct.unpack_from("<I", packet, 8)[0]
    offset = 12 + vendor
    if offset + 4 > len(packet):
        raise IntelligenceError("unsupported_audio", 415)
    comments = struct.unpack_from("<I", packet, offset)[0]
    offset += 4
    for _ in range(comments):
        if offset + 4 > len(packet):
            raise IntelligenceError("unsupported_audio", 415)
        length = struct.unpack_from("<I", packet, offset)[0]
        offset += 4 + length
        if offset > len(packet):
            raise IntelligenceError("unsupported_audio", 415)
    if offset != len(packet):
        raise IntelligenceError("unsupported_audio", 415)


def validate_opus(file):
    file.seek(0)
    size = os.fstat(file.fileno()).st_size
    if not 1 <= size <= MAX_OPUS_BYTES:
        raise IntelligenceError("unsupported_audio", 415)
    serial = sequence = packet_index = total_48k = None
    pre_skip = prior_granule = 0
    eos = False
    while file.tell() < size:
        header = file.read(27)
        if (len(header) != 27 or header[:4] != b"OggS" or header[4] != 0
                or header[5] & ~7 or eos):
            raise IntelligenceError("unsupported_audio", 415)
        flags, granule, page_serial, page_sequence = (
            header[5], struct.unpack_from("<Q", header, 6)[0],
            struct.unpack_from("<I", header, 14)[0], struct.unpack_from("<I", header, 18)[0])
        if sequence is None:
            if not flags & 2 or page_sequence != 0:
                raise IntelligenceError("unsupported_audio", 415)
            serial, sequence, packet_index, total_48k = page_serial, 0, 0, 0
        if (flags & 1 or page_serial != serial or page_sequence != sequence
                or (sequence and flags & 2)):
            raise IntelligenceError("unsupported_audio", 415)
        lacing = file.read(header[26])
        body = file.read(sum(lacing))
        if len(lacing) != header[26] or len(body) != sum(lacing) or (lacing and lacing[-1] == 255):
            raise IntelligenceError("unsupported_audio", 415)
        checked = bytearray(header + lacing + body)
        expected_crc = struct.unpack_from("<I", checked, 22)[0]
        checked[22:26] = b"\0\0\0\0"
        if _ogg_crc(checked) != expected_crc:
            raise IntelligenceError("unsupported_audio", 415)
        start = cursor = 0
        for lace in lacing:
            cursor += lace
            if lace == 255:
                continue
            packet = body[start:cursor]
            if packet_index == 0:
                if (sequence != 0 or packet[:8] != b"OpusHead" or len(packet) != 19
                        or packet[8:10] != b"\x01\x01"
                        or struct.unpack_from("<I", packet, 12)[0] != 16000
                        or packet[16:] != b"\0\0\0"):
                    raise IntelligenceError("unsupported_audio", 415)
                pre_skip = struct.unpack_from("<H", packet, 10)[0]
                if not pre_skip:
                    raise IntelligenceError("unsupported_audio", 415)
            elif packet_index == 1:
                if sequence != 1 or cursor != len(body):
                    raise IntelligenceError("unsupported_audio", 415)
                _validate_opus_tags(packet)
            else:
                total_48k += _opus_packet_samples(packet)
            packet_index += 1
            start = cursor
        if packet_index > 2:
            expected = total_48k
            if ((not flags & 4 and granule != expected)
                    or (flags & 4 and (granule > total_48k or granule < prior_granule))):
                raise IntelligenceError("unsupported_audio", 415)
        elif granule != 0:
            raise IntelligenceError("unsupported_audio", 415)
        eos = bool(flags & 4)
        if packet_index > 2:
            prior_granule = granule
        sequence += 1
    if packet_index is None or packet_index < 3 or not eos:
        raise IntelligenceError("unsupported_audio", 415)
    output_48k = prior_granule - pre_skip
    if output_48k <= 0 or output_48k % 3:
        raise IntelligenceError("unsupported_audio", 415)
    frames = output_48k // 3
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
        path = Path(f".recording-{uuid4().hex}.opus")
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

            await graph.download(request.drive_id, request.item_id, MAX_OPUS_BYTES, sink)
            await report("validating")
            await disk(file.flush)
            if digest.hexdigest() != request.source_sha1:
                raise IntelligenceError("source_changed", 409)
            await disk(validate_opus, file)
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
