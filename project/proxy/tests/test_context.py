import hashlib
from pathlib import Path
import struct

import pytest

from recorder_proxy.context import ContextError, decode_context
from recorder_proxy.protocol import CONTEXT_HEADER, ContextChunk, ProtocolError


FIXTURE = Path(__file__).parent.joinpath("fixtures", "context-16k.ogg").read_bytes()


def test_independently_encoded_ogg_opus_decodes_exactly():
    pcm = decode_context(FIXTURE)
    assert len(pcm) == 64000
    assert hashlib.sha256(pcm).hexdigest() == (
        "4f7988030a00d082fe445e00a2ac5dab502300ff1b80e8592dd569867b60ef74"
    )
    assert pcm == bytes(64000)


@pytest.mark.parametrize("mutate", [
    lambda data: data[:20],
    lambda data: data.replace(b"OpusHead", b"NotOpus!", 1),
    lambda data: data[:data.find(b"OpusHead") + 12] + struct.pack("<I", 48000)
    + data[data.find(b"OpusHead") + 16:],
    lambda data: data + data,
])
def test_malformed_or_unsupported_context_is_rejected(mutate):
    with pytest.raises(ContextError):
        decode_context(mutate(FIXTURE))


def test_empty_context_skips_decoding():
    assert decode_context(b"") == b""


def test_context_chunk_wire_limit_and_declared_length():
    chunk = ContextChunk(4, 123, bytes(4076))
    assert ContextChunk.parse(chunk.encode()) == chunk
    with pytest.raises(ProtocolError):
        ContextChunk(0, 0, bytes(4077)).encode()
    wire = bytearray(ContextChunk(0, 0, b"x").encode())
    CONTEXT_HEADER.pack_into(wire, 0, b"ERC2", 2, 1, 0, 0, 0, 2)
    with pytest.raises(ProtocolError):
        ContextChunk.parse(bytes(wire))
