import hashlib
import json
import struct
from types import SimpleNamespace

import httpx


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


def _ogg_page(serial, sequence, flags, granule, packets):
    lacing = bytearray()
    body = b""
    for packet in packets:
        remaining = len(packet)
        while remaining >= 255:
            lacing.append(255)
            remaining -= 255
        lacing.append(remaining)
        body += packet
    header = bytearray(
        b"OggS\0" + bytes([flags]) + struct.pack("<QII", granule, serial, sequence)
        + b"\0\0\0\0" + bytes([len(lacing)]) + lacing
    )
    data = header + body
    struct.pack_into("<I", data, 22, _ogg_crc(data))
    return bytes(data)


def opus_bytes(frames=320, rate=16000, channels=1, width=2):
    serial, pre_skip = 0x12345678, 312
    head = b"OpusHead" + bytes([1, channels]) + struct.pack("<HIhB", pre_skip, rate, 0, 0)
    if width != 2:
        head = head[:-1] + b"\1"
    tags = b"OpusTags" + struct.pack("<I", 8) + b"recorder" + struct.pack("<I", 0)
    packets = max(1, (frames + 319) // 320)
    pages = [_ogg_page(serial, 0, 2, 0, [head]),
             _ogg_page(serial, 1, 0, 0, [tags])]
    encoded_packets = packets + 1
    for start in range(0, encoded_packets, 50):
        count = min(50, encoded_packets - start)
        final = start + count == encoded_packets
        granule = pre_skip + packets * 960 if final else (start + count) * 960
        pages.append(_ogg_page(serial, len(pages), 4 if final else 0, granule,
                               [b"\x48" + bytes(59)] * count))
    return b"".join(pages)


class Credential:
    def __init__(self):
        self.scopes = []
        self.closed = False

    async def get_token(self, scope):
        self.scopes.append(scope)
        return SimpleNamespace(token=f"managed-assertion-{len(self.scopes)}", expires_on=9999999999)

    async def close(self):
        self.closed = True


class Tokens:
    def __init__(self):
        self.refreshes = []

    async def token(self, user, *, refresh=False):
        self.refreshes.append(refresh)
        return "delegated-graph"


class Drive:
    """HTTP fixtures, not replacement Graph/processing implementations."""

    def __init__(self, content=None):
        self.content = {}
        self.items = {}
        self.calls = []
        self.writes = []
        self.fail_json = False
        self.redirect = None
        self.next_link = None
        self.paginated = False
        self.add("root", "root", None, folder=True)
        self.add("folder", "local-recording", "root", folder=True)
        self.add("audio", "AudioRecording_20260907_220536.opus", "folder",
                 opus_bytes() if content is None else content)

    def add(self, id, name, parent, content=b"", folder=False):
        item = {"id": id, "name": name, "eTag": f'"{id}-v1"', "size": len(content),
                "parentReference": {"driveId": "drive", "id": parent},
                "lastModifiedDateTime": "2026-09-07T14:05:36Z"}
        if folder:
            item["folder"] = {}
        else:
            item["file"] = {"hashes": {"sha1Hash": hashlib.sha1(content).hexdigest()}}
            self.content[id] = content
        self.items[id] = item
        return item

    async def handle(self, request):
        self.calls.append(request)
        if request.url.host != "graph.microsoft.com":
            assert "authorization" not in request.headers
            return httpx.Response(200, content=self.content["audio"])
        assert request.headers["authorization"] == "Bearer delegated-graph"
        path = request.url.path.removeprefix("/v1.0")
        if path == "/me/drive":
            return httpx.Response(200, json={"id": "drive", "driveType": "personal"})
        if path == "/drives/drive/root":
            return httpx.Response(200, json=self.items["root"])
        if not path.startswith("/drives/drive/items/"):
            return httpx.Response(404)
        suffix = path.removeprefix("/drives/drive/items/")
        if ":/" in suffix:
            parent, name = suffix.split(":/", 1)
            name = name.removesuffix(":/content")
            found = next((item for item in self.items.values()
                          if item["parentReference"]["id"] == parent and item["name"] == name), None)
            if request.method == "PUT":
                assert request.headers["if-none-match"] == "*"
                assert request.url.params["@microsoft.graph.conflictBehavior"] == "fail"
                if found:
                    return httpx.Response(409)
                if self.fail_json and name.endswith(".json"):
                    return httpx.Response(503, json={"error": "sensitive provider message"})
                id = f"output-{len(self.writes)}"
                body = await request.aread()
                found = self.add(id, name, parent, body)
                self.writes.append(name)
            return httpx.Response(200, json=found) if found else httpx.Response(404)
        if suffix.endswith("/children"):
            parent = suffix.removesuffix("/children")
            items = [item for item in self.items.values() if item["parentReference"]["id"] == parent]
            offset = int(request.url.params.get("$skiptoken", 0)) if self.paginated else 0
            result = {"value": items[offset:offset + 10]}
            if self.next_link:
                result["@odata.nextLink"] = self.next_link
            elif self.paginated and len(items) > offset + 10:
                result["@odata.nextLink"] = (
                    f"https://graph.microsoft.com/v1.0/drives/drive/items/{parent}/children"
                    f"?$skiptoken={offset + 10}")
            return httpx.Response(200, json=result)
        if suffix.endswith("/content"):
            id = suffix.removesuffix("/content")
            if self.redirect and id == "audio":
                return httpx.Response(302, headers={"Location": self.redirect})
            return httpx.Response(200, content=self.content[id]) if id in self.content else httpx.Response(404)
        return (httpx.Response(200, json=self.items[suffix])
                if suffix in self.items else httpx.Response(404))


class SpeechService:
    def __init__(self, phrases=None):
        self.calls = []
        self.status = 200
        self.wait = None
        self.document = {
            "durationMilliseconds": 10, "combinedPhrases": [],
            "phrases": phrases if phrases is not None else [
                {"offsetMilliseconds": 0, "durationMilliseconds": 10,
                 "locale": "zh-CN", "text": "你好 hello."}],
        }

    async def handle(self, request):
        self.calls.append(request)
        assert request.url.path == "/speechtotext/transcriptions:transcribe"
        assert request.url.params["api-version"] == "2025-10-15"
        assert request.headers["authorization"].startswith("Bearer managed-assertion-")
        body = await request.aread()
        assert b'name="definition"' in body and b'\r\n\r\n{"locales":[]}\r\n' in body
        assert b'name="audio"' in body and b"OggS" in body
        assert b"Content-Type: audio/ogg; codecs=opus" in body
        assert b"audioUrl" not in body
        assert len(body) == int(request.headers["content-length"])
        if self.wait:
            await self.wait.wait()
        return httpx.Response(self.status, json=self.document)
