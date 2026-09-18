from dataclasses import dataclass
import json
import struct


HEADER = struct.Struct("<4sBBHIIQ")
SUBPROTOCOL = "recorder.voice.v1"
SUBPROTOCOL_V2 = "recorder.voice.v2"
CONTEXT_HEADER = struct.Struct("<4sBBHIII")
MAX_CONTROL = 4096
MAX_FRAME = 664
MAX_CONTEXT_WIRE = 4096
MAX_CONTEXT_BYTES = 256 * 1024
MAX_U32 = (1 << 32) - 1
MAX_U64 = (1 << 64) - 1


class ProtocolError(Exception):
    """Malformed device message. Never includes device-provided content."""


def integer(value, maximum):
    return type(value) is int and 0 <= value <= maximum


@dataclass(frozen=True)
class Frame:
    kind: int
    epoch: int
    sequence: int
    position: int
    pcm: bytes

    @classmethod
    def parse(cls, data, expected_kind=1):
        if not 26 <= len(data) <= MAX_FRAME or len(data) % 2:
            raise ProtocolError("Invalid PCM frame length")
        magic, version, kind, reserved, epoch, sequence, position = HEADER.unpack_from(data)
        if magic != b"ERV1" or version != 1 or reserved or kind != expected_kind:
            raise ProtocolError("Invalid PCM frame header")
        if (kind == 1 and epoch != 0) or (kind == 2 and epoch == 0):
            raise ProtocolError("Invalid audio epoch")
        return cls(kind, epoch, sequence, position, data[24:])

    def encode(self):
        data = HEADER.pack(b"ERV1", 1, self.kind, 0, self.epoch, self.sequence, self.position) + self.pcm
        self.parse(data, self.kind)
        return data


@dataclass(frozen=True)
class ContextChunk:
    sequence: int
    offset: int
    data: bytes

    @classmethod
    def parse(cls, wire):
        if not CONTEXT_HEADER.size < len(wire) <= MAX_CONTEXT_WIRE:
            raise ProtocolError("Invalid context chunk length")
        magic, version, kind, reserved, sequence, offset, length = CONTEXT_HEADER.unpack_from(wire)
        if (magic != b"ERC2" or version != 2 or kind != 1 or reserved
                or length != len(wire) - CONTEXT_HEADER.size or length == 0):
            raise ProtocolError("Invalid context chunk")
        return cls(sequence, offset, wire[CONTEXT_HEADER.size:])

    def encode(self):
        wire = CONTEXT_HEADER.pack(
            b"ERC2", 2, 1, 0, self.sequence, self.offset, len(self.data),
        ) + self.data
        if not self.data or len(wire) > MAX_CONTEXT_WIRE:
            raise ProtocolError("Invalid context chunk length")
        return wire


class Sequence:
    def __init__(self):
        self.sequence = 0
        self.position = 0

    def accept(self, frame):
        if frame.sequence != self.sequence or frame.position != self.position:
            raise ProtocolError("Audio sequence or sample position mismatch")
        self.sequence += 1
        self.position += len(frame.pcm) // 2
        if self.sequence > MAX_U32 or self.position > MAX_U64:
            raise ProtocolError("Audio stream exhausted")


def _unique_object(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ProtocolError("Duplicate JSON field")
        obj[key] = value
    return obj


def parse_control(text, expected_version=1):
    if len(text.encode("utf-8")) > MAX_CONTROL:
        raise ProtocolError("Control exceeds limit")
    try:
        data = json.loads(text, object_pairs_hook=_unique_object)
    except (ValueError, RecursionError):
        raise ProtocolError("Invalid JSON control") from None
    if (not isinstance(data, dict) or type(data.get("v")) is not int
            or data["v"] != expected_version):
        raise ProtocolError("Invalid protocol version")
    kind = data.get("type")
    if kind == "hello":
        expected = {"v": expected_version, "type": "hello", "sample_rate": 16000, "channels": 1,
                    "format": "pcm16", "frame_samples": 320}
        if expected_version == 2:
            context = data.get("context")
            if (not isinstance(context, dict)
                    or set(context) != {"format", "length", "sha256"}
                    or context.get("format") != "ogg_opus"
                    or not integer(context.get("length"), MAX_CONTEXT_BYTES)
                    or not isinstance(context.get("sha256"), str)
                    or len(context["sha256"]) != 64
                    or any(c not in "0123456789abcdef" for c in context["sha256"])):
                raise ProtocolError("Invalid context metadata")
            expected["context"] = context
        if data != expected or any(type(data.get(k)) is not int for k in (
            "sample_rate", "channels", "frame_samples",
        )):
            raise ProtocolError("Unsupported audio capabilities")
    elif kind in ("playback.progress", "playback.cleared"):
        if (set(data) != {"v", "type", "epoch", "played_samples"}
                or not integer(data["epoch"], MAX_U32) or data["epoch"] == 0
                or not integer(data["played_samples"], MAX_U64)):
            raise ProtocolError("Invalid playback acknowledgment")
    elif kind == "stop":
        if (set(data) - {"v", "type", "reason"}
                or ("reason" in data and (not isinstance(data["reason"], str)
                                         or len(data["reason"]) > 256))):
            raise ProtocolError("Invalid stop")
    else:
        raise ProtocolError("Unknown control type")
    return data


def control(kind, *, version=1, **fields):
    return {"v": version, "type": kind, **fields}
