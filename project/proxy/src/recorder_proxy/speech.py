import asyncio
import json
import os
import re
from uuid import uuid4

from azure.core.exceptions import AzureError
from azure.identity.aio import ManagedIdentityCredential
import httpx

from .intelligence_errors import IntelligenceError, http_client, json_body, unavailable

SPEECH_VERSION = "2025-10-15"
SPEECH_SCOPE = "https://cognitiveservices.azure.com/.default"


async def disk(function, *args):
    # Do not close/unlink the WAV while an in-flight filesystem operation uses it.
    task = asyncio.create_task(asyncio.to_thread(function, *args))
    try:
        return await asyncio.shield(task)
    except asyncio.CancelledError:
        await task
        raise


class AudioMultipart(httpx.AsyncByteStream):
    def __init__(self, file, size):
        self.file = file
        self.boundary = uuid4().hex
        self.prefix = (
            f"--{self.boundary}\r\nContent-Disposition: form-data; name=\"definition\"\r\n"
            'Content-Type: application/json\r\n\r\n{"locales":[]}\r\n'
            f"--{self.boundary}\r\nContent-Disposition: form-data; name=\"audio\"; "
            'filename="recording.wav"\r\nContent-Type: audio/wav\r\n\r\n'
        ).encode()
        self.suffix = f"\r\n--{self.boundary}--\r\n".encode()
        self.size = len(self.prefix) + size + len(self.suffix)

    async def __aiter__(self):
        yield self.prefix
        await disk(self.file.seek, 0)
        while chunk := await disk(self.file.read, 65536):
            yield chunk
        yield self.suffix


def normalize_transcript(document):
    duration = document.get("durationMilliseconds")
    combined, phrases = document.get("combinedPhrases"), document.get("phrases")
    if (type(duration) not in (int, float) or not 0 <= duration <= 1801000
            or not isinstance(combined, list) or not isinstance(phrases, list)
            or len(combined) > 2 or len(phrases) > 20000):
        raise unavailable()
    result = []
    for phrase in phrases:
        if not isinstance(phrase, dict):
            raise unavailable()
        text = phrase.get("text")
        offset, length = phrase.get("offsetMilliseconds"), phrase.get("durationMilliseconds")
        locale = phrase.get("locale", "und")
        if (not isinstance(text, str) or len(text.encode("utf-8")) > 262144
                or type(offset) not in (int, float) or type(length) not in (int, float)
                or not 0 <= offset <= 1800000 or not 0 <= length <= 1801000
                or offset + length > 1801000
                or not isinstance(locale, str) or not re.fullmatch(r"[A-Za-z0-9-]{1,35}", locale)):
            raise unavailable()
        text = " ".join(text.split())
        if text:
            result.append({"offset_ms": int(offset), "duration_ms": int(length),
                           "locale": locale, "text": text})
    combined_text = []
    for phrase in combined:
        if not isinstance(phrase, dict) or not isinstance(phrase.get("text"), str):
            raise unavailable()
        combined_text.append(phrase["text"])
    if not result and any(text.strip() for text in combined_text):
        result = [{"offset_ms": 0, "duration_ms": int(duration), "locale": "und",
                   "text": " ".join(" ".join(combined_text).split())}]
    if len(json.dumps(result, ensure_ascii=False).encode()) > 2 * 1024 * 1024:
        raise unavailable()
    return result


class FastTranscription:
    def __init__(self, settings, *, http=None, credential=None):
        self.settings = settings
        self.http = http or http_client()
        self.credential = credential or ManagedIdentityCredential(
            client_id=settings.managed_identity_client_id)

    async def transcribe(self, file):
        multipart = AudioMultipart(file, await disk(lambda: os.fstat(file.fileno()).st_size))
        try:
            token = await self.credential.get_token(SPEECH_SCOPE)
            async with self.http.stream(
                "POST", self.settings.speech_endpoint +
                f"/speechtotext/transcriptions:transcribe?api-version={SPEECH_VERSION}",
                headers={"Authorization": f"Bearer {token.token}",
                         "Content-Type": f"multipart/form-data; boundary={multipart.boundary}",
                         "Content-Length": str(multipart.size)},
                content=multipart, timeout=httpx.Timeout(180, connect=5),
            ) as response:
                if response.status_code == 429:
                    raise IntelligenceError("busy", 429, True)
                if response.status_code == 413:
                    raise IntelligenceError("recording_too_long", 413)
                if response.status_code in (400, 415):
                    raise IntelligenceError("unsupported_audio", 415)
                if response.status_code != 200:
                    raise unavailable()
                document = await json_body(response, 6 * 1024 * 1024)
                return normalize_transcript(document)
        except (AzureError, httpx.HTTPError):
            raise unavailable() from None

    async def close(self):
        try:
            await self.http.aclose()
        finally:
            await self.credential.close()
