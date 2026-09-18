from io import BytesIO
import struct

import av


MAX_PCM_BYTES = 960000


class ContextError(Exception):
    """Invalid audio context. Carries no input-derived details."""


def _ogg_crc(page):
    crc = 0
    for byte in page:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF
    return crc


def _validate_opus_head(data):
    offset = 0
    serial = None
    expected_sequence = 0
    first_packet = None
    saw_eos = False
    while offset < len(data):
        if len(data) - offset < 27 or data[offset:offset + 4] != b"OggS":
            raise ContextError()
        segment_count = data[offset + 26]
        header_end = offset + 27 + segment_count
        if header_end > len(data):
            raise ContextError()
        sizes = data[offset + 27:header_end]
        page_end = header_end + sum(sizes)
        if page_end > len(data):
            raise ContextError()
        page = bytearray(data[offset:page_end])
        expected_crc = struct.unpack_from("<I", page, 22)[0]
        page[22:26] = bytes(4)
        if _ogg_crc(page) != expected_crc:
            raise ContextError()
        flags = data[offset + 5]
        page_serial = struct.unpack_from("<I", data, offset + 14)[0]
        sequence = struct.unpack_from("<I", data, offset + 18)[0]
        if (data[offset + 4] != 0 or saw_eos or sequence != expected_sequence
                or (offset == 0) != bool(flags & 2)
                or (serial is not None and page_serial != serial)):
            raise ContextError()
        serial = page_serial
        expected_sequence += 1
        if first_packet is None:
            packet_length = 0
            for size in sizes:
                packet_length += size
                if size < 255:
                    break
            else:
                raise ContextError()
            first_packet = data[header_end:header_end + packet_length]
        saw_eos = bool(flags & 4)
        offset = page_end
    if not saw_eos or first_packet is None:
        raise ContextError()
    packet = first_packet
    if (len(packet) < 19 or packet[:8] != b"OpusHead" or packet[8] != 1
            or packet[9] != 1 or struct.unpack_from("<I", packet, 12)[0] != 16000
            or packet[18] != 0):
        raise ContextError()


def decode_context(data):
    """Decode an in-memory mono, 16 kHz-origin Ogg Opus stream to PCM16/16k."""
    if not data:
        return b""
    _validate_opus_head(data)
    output = bytearray()
    try:
        with av.open(BytesIO(data), mode="r", format="ogg") as container:
            audio = [stream for stream in container.streams if stream.type == "audio"]
            if len(container.streams) != 1 or len(audio) != 1 or audio[0].codec_context.name != "opus":
                raise ContextError()
            resampler = av.AudioResampler(format="s16", layout="mono", rate=16000)

            def append(frames):
                for frame in frames:
                    if frame.layout.name != "mono":
                        raise ContextError()
                    pcm = frame.to_ndarray().astype("<i2", copy=False).tobytes()
                    if not pcm or len(output) + len(pcm) > MAX_PCM_BYTES:
                        raise ContextError()
                    output.extend(pcm)

            for frame in container.decode(audio[0]):
                if frame.layout.name != "mono":
                    raise ContextError()
                append(resampler.resample(frame))
            append(resampler.resample(None))
    except ContextError:
        raise
    except (av.FFmpegError, EOFError, OSError, OverflowError, ValueError):
        raise ContextError() from None
    if not output or len(output) % 2:
        raise ContextError()
    return bytes(output)
