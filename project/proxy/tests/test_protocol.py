import json
from pathlib import Path
import struct

import pytest

from recorder_proxy.protocol import Frame, HEADER, ProtocolError, Sequence, parse_control


SHARED_FIXTURES = json.loads(
    Path(__file__).resolve().parents[2].joinpath(
        "protocols", "voice-v1-fixtures.json"
    ).read_text(encoding="utf-8")
)


@pytest.mark.parametrize("fixture", SHARED_FIXTURES["valid"], ids=lambda case: case["name"])
def test_shared_valid_wire_fixture(fixture):
    wire = bytes.fromhex(fixture["hex"])
    frame = Frame.parse(wire, expected_kind=fixture["kind"])
    assert (frame.kind, frame.epoch, frame.sequence, frame.position) == (
        fixture["kind"], fixture["epoch"], fixture["sequence"], fixture["sample_position"],
    )
    assert list(struct.unpack(f"<{len(frame.pcm) // 2}h", frame.pcm)) == fixture["samples"]
    assert frame.encode() == wire


@pytest.mark.parametrize("fixture", SHARED_FIXTURES["invalid"], ids=lambda case: case["name"])
def test_shared_invalid_wire_fixture(fixture):
    with pytest.raises(ProtocolError):
        Frame.parse(bytes.fromhex(fixture["hex"]))


def test_exact_binary_fixture():
    frame = Frame(1, 0, 0, 0, b"\x34\x12\x00\x80")
    assert HEADER.size == 24
    assert frame.encode().hex() == "45525631010100000000000000000000000000000000000034120080"
    assert Frame.parse(frame.encode()) == frame
    state = Sequence()
    state.accept(frame)
    state.accept(Frame(1, 0, 1, 2, b"\0\0"))
    assert (state.sequence, state.position) == (2, 3)


@pytest.mark.parametrize("offset,value", [(0, 0), (4, 2), (5, 2), (6, 1), (8, 1)])
def test_bad_header(offset, value):
    data = bytearray(Frame(1, 0, 0, 0, b"\0\0").encode())
    data[offset] = value
    with pytest.raises(ProtocolError):
        Frame.parse(bytes(data))


@pytest.mark.parametrize("payload", [b"", b"\0", bytes(641), bytes(642)])
def test_bad_payload(payload):
    data = struct.pack("<4sBBHIIQ", b"ERV1", 1, 1, 0, 0, 0, 0) + payload
    with pytest.raises(ProtocolError):
        Frame.parse(data)


@pytest.mark.parametrize("seq,pos", [(1, 0), (0, 1), (100, 100)])
def test_sequence(seq, pos):
    with pytest.raises(ProtocolError):
        Sequence().accept(Frame(1, 0, seq, pos, b"\0\0"))


@pytest.mark.parametrize("data", [
    {"v": True, "type": "stop"}, {"v": 1, "type": "hello"},
    {"v": 1, "type": "tool.call"}, {"v": 1, "type": "ready"},
    {"v": 1, "type": "playback.progress", "epoch": 1, "played_samples": -1},
    {"v": 1, "type": "playback.cleared", "epoch": True, "played_samples": 0},
    {"v": 1, "type": "stop", "reason": {}},
])
def test_bad_control(data):
    with pytest.raises(ProtocolError):
        parse_control(json.dumps(data))


@pytest.mark.parametrize("text", ['{"v":1,"v":1,"type":"stop"}', "null", "[1]", "{" * 4097])
def test_malformed_control(text):
    with pytest.raises(ProtocolError):
        parse_control(text)
