import hashlib
import io
import json
from types import SimpleNamespace
import wave

import httpx


def wav_bytes(frames=160, rate=16000, channels=1, width=2):
    file = io.BytesIO()
    with wave.open(file, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(width)
        wav.setframerate(rate)
        wav.writeframes(bytes(frames * channels * width))
    return file.getvalue()


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
        self.add("audio", "AudioRecording_20260907_220536.wav", "folder",
                 wav_bytes() if content is None else content)

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
        assert b'name="audio"' in body and b"RIFF" in body
        assert b"audioUrl" not in body
        assert len(body) == int(request.headers["content-length"])
        if self.wait:
            await self.wait.wait()
        return httpx.Response(self.status, json=self.document)
