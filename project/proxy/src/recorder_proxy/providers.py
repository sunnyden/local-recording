import asyncio
import base64
import binascii
from dataclasses import dataclass
import json
from typing import AsyncIterator, Protocol
from urllib.parse import urlencode

from azure.core.exceptions import AzureError
from azure.identity.aio import ManagedIdentityCredential
from websockets.asyncio.client import connect
from websockets.exceptions import WebSocketException

from .config import API_VERSION
from .tools import TOOL_PROMPT, tool_definitions


class ProviderError(Exception):
    """Redacted provider boundary failure."""


@dataclass(frozen=True)
class Event:
    type: str
    response_id: str = ""
    item_id: str = ""
    content_index: int = 0
    pcm: bytes = b""
    call_id: str = ""
    name: str = ""
    arguments: str = ""
    event_id: str = ""


class VoiceProvider(Protocol):
    sample_rate: int

    async def open(self): ...
    async def append(self, pcm: bytes): ...
    async def respond(self): ...
    def events(self) -> AsyncIterator[Event]: ...
    async def truncate(self, item_id: str, content_index: int, played_samples: int): ...
    async def close(self): ...


def session_configuration(tools_enabled=False):
    result = {
        "type": "session.update",
        "session": {
            "modalities": ["text", "audio"],
            "input_audio_format": "pcm16",
            "input_audio_sampling_rate": 16000,
            "output_audio_format": "pcm16",
            "input_audio_echo_cancellation": {"type": "server_echo_cancellation"},
            "turn_detection": {"type": "azure_semantic_vad_multilingual",
                               "create_response": False, "interrupt_response": True},
            "tools": [],
            "tool_choice": "none",
        },
    }
    if tools_enabled:
        result["session"].update(tools=tool_definitions(), tool_choice="auto",
                                 instructions=TOOL_PROMPT)
    return result


def verify_configuration(message, settings):
    session = message.get("session", {})
    requested = session_configuration(settings.onedrive_tools_enabled)["session"]
    if not isinstance(session, dict):
        raise ProviderError()
    if settings.onedrive_tools_enabled and session.get("instructions") != TOOL_PROMPT:
        raise ProviderError()
    for key in ("input_audio_format", "input_audio_sampling_rate", "output_audio_format",
                "tools", "tool_choice"):
        if type(session.get(key)) is not type(requested[key]) or session[key] != requested[key]:
            raise ProviderError()
    echo = session.get("input_audio_echo_cancellation")
    if (not isinstance(echo, dict) or echo.get("type") != "server_echo_cancellation"
            or echo.get("reference_source", "server") != "server"
            or type(echo.get("channels", 1)) is not int or echo.get("channels", 1) != 1):
        raise ProviderError()
    vad = session.get("turn_detection")
    if not isinstance(vad, dict) or any(
        type(vad.get(k)) is not type(v) or vad[k] != v
        for k, v in requested["turn_detection"].items()
    ):
        raise ProviderError()
    accepted_models = (settings.model,)
    if settings.profile == "native" and settings.model == "gpt-realtime-2":
        # Observed native Voice Live canonical name; never apply to BYOM deployments.
        accepted_models += ("gpt-realtime-2-global-standard",)
    if session.get("model") not in accepted_models:
        raise ProviderError()
    if not isinstance(session.get("modalities"), list) or "audio" not in session["modalities"]:
        raise ProviderError()


def _identifier(data, key):
    value = data.get(key)
    if not isinstance(value, str) or not 1 <= len(value) <= 256:
        raise ProviderError()
    return value


def map_event(data, tools_enabled=False):
    kind = data.get("type")
    if not isinstance(kind, str):
        raise ProviderError()
    if kind == "error":
        raise ProviderError()
    if kind in ("response.function_call_arguments.delta", "response.function_call_arguments.done"):
        if tools_enabled:
            return tool_event(data)
        raise ProviderError()
    if kind in ("response.output_item.added", "response.output_item.done"):
        item = data.get("item", {})
        if tools_enabled and isinstance(item, dict) and item.get("type") == "function_call":
            return tool_event(data)
        if not isinstance(item, dict) or item.get("type") == "function_call":
            raise ProviderError()
    if kind == "input_audio_buffer.speech_started":
        return Event("speech_started")
    if kind == "input_audio_buffer.speech_stopped":
        return Event("speech_stopped")
    if kind == "input_audio_buffer.committed":
        return Event("input_committed")
    if kind in ("response.audio.delta", "response.audio.done"):
        rid, iid = _identifier(data, "response_id"), _identifier(data, "item_id")
        index = data.get("content_index")
        if type(index) is not int or not 0 <= index <= 16:
            raise ProviderError()
        pcm = b""
        if kind.endswith(".delta"):
            encoded = data.get("delta")
            if not isinstance(encoded, str) or not encoded or len(encoded) > 87384:
                raise ProviderError()
            try:
                pcm = base64.b64decode(encoded, validate=True)
            except (binascii.Error, ValueError):
                raise ProviderError() from None
            if not pcm or len(pcm) > 65536 or len(pcm) % 2:
                raise ProviderError()
        return Event("audio" if pcm else "audio_done", rid, iid, index, pcm)
    if kind == "response.done":
        response = data.get("response")
        if not isinstance(response, dict) or response.get("status") not in ("completed", "cancelled"):
            raise ProviderError()
        output = response.get("output", [])
        if not isinstance(output, list) or len(output) > 128:
            raise ProviderError()
        for item in output:
            if not isinstance(item, dict) or (item.get("type") == "function_call" and not tools_enabled):
                raise ProviderError()
        return Event("response_done", _identifier(response, "id"))
    if kind == "response.created":
        response = data.get("response")
        if not isinstance(response, dict):
            raise ProviderError()
        return Event("response_started", _identifier(response, "id"))
    # Known metadata/transcript events are intentionally discarded, never logged.
    if kind in {
        "session.created", "session.updated",
        "input_audio_buffer.cleared", "conversation.item.created", "conversation.item.truncated",
        "conversation.item.deleted", "conversation.item.input_audio_transcription.completed",
        "conversation.item.input_audio_transcription.delta", "rate_limits.updated",
        "response.output_item.added", "response.output_item.done",
        "response.content_part.added", "response.content_part.done",
        "response.audio_transcript.delta", "response.audio_transcript.done",
        "response.text.delta", "response.text.done",
    }:
        return None
    raise ProviderError()


def tool_event(data):
    kind = data["type"]
    item = data.get("item", {})
    if not isinstance(item, dict):
        raise ProviderError()
    call_id = item.get("call_id", data.get("call_id", ""))
    item_id = item.get("id", data.get("item_id", ""))
    name = item.get("name", data.get("name", ""))
    arguments = data.get("delta") if kind.endswith(".delta") else item.get(
        "arguments", data.get("arguments", ""))
    event_id = data.get("event_id", "")
    if (not isinstance(call_id, str) or len(call_id) > 256
            or not isinstance(item_id, str) or len(item_id) > 256
            or not (call_id or item_id)
            or not isinstance(name, str) or len(name) > 128
            or not isinstance(arguments, str) or len(arguments.encode()) > 8192
            or not isinstance(event_id, str) or len(event_id) > 256):
        raise ProviderError()
    event_type = ("tool_delta" if kind.endswith(".delta") else
                  "tool_added" if kind.endswith(".added") else "tool_done")
    return Event(event_type, _identifier(data, "response_id"), item_id,
                 call_id=call_id, name=name, arguments=arguments, event_id=event_id)


class VoiceLive:
    sample_rate = 24000  # Voice Live pcm16 is 24 kHz; not OpenAI GA audio.format.

    def __init__(self, settings):
        self.settings = settings
        self.credential = ManagedIdentityCredential(client_id=settings.managed_identity_client_id)
        self.socket = None
        self.send_lock = asyncio.Lock()

    async def _send(self, data):
        try:
            async with self.send_lock:
                async with asyncio.timeout(5):
                    await self.socket.send(json.dumps(data, separators=(",", ":")))
        except (WebSocketException, OSError, TimeoutError):
            raise ProviderError() from None

    async def _receive(self):
        try:
            text = await self.socket.recv()
            if not isinstance(text, str):
                raise ProviderError()
            data = json.loads(text)
        except (WebSocketException, OSError, ValueError, RecursionError):
            raise ProviderError() from None
        if not isinstance(data, dict):
            raise ProviderError()
        return data

    async def open(self):
        query = {"api-version": API_VERSION, "model": self.settings.model}
        if self.settings.profile != "native":
            query["profile"] = self.settings.profile
        url = self.settings.endpoint.replace("https://", "wss://", 1)
        url += "/voice-live/realtime?" + urlencode(query)
        try:
            async with asyncio.timeout(self.settings.handshake_seconds):
                token = await self.credential.get_token("https://ai.azure.com/.default")
                self.socket = await connect(
                    url, additional_headers={"Authorization": f"Bearer {token.token}"},
                    max_size=131072, max_queue=4, write_limit=32768,
                    ping_interval=20, ping_timeout=10, close_timeout=2,
                    open_timeout=10, compression=None, proxy=None,
                )
                del token
                if (await self._receive()).get("type") != "session.created":
                    raise ProviderError()
                await self._send(session_configuration(self.settings.onedrive_tools_enabled))
                message = await self._receive()
                if message.get("type") != "session.updated":
                    raise ProviderError()
                verify_configuration(message, self.settings)
        except (AzureError, WebSocketException, OSError, TimeoutError):
            raise ProviderError() from None

    async def append(self, pcm):
        if not 2 <= len(pcm) <= 640 or len(pcm) % 2:
            raise ProviderError()
        await self._send({"type": "input_audio_buffer.append",
                          "audio": base64.b64encode(pcm).decode("ascii")})

    async def events(self):
        while True:
            data = await self._receive()
            if self.settings.onedrive_tools_enabled and data.get("type") == "response.done":
                response = data.get("response")
                if (not isinstance(response, dict) or not isinstance(response.get("output", []), list)
                        or len(response.get("output", [])) > 128):
                    raise ProviderError()
                if response.get("status") == "completed":
                    for item in response.get("output", []):
                        if isinstance(item, dict) and item.get("type") == "function_call":
                            yield tool_event({"type": "response.output_item.done",
                                              "response_id": response.get("id"), "item": item})
            event = map_event(data, self.settings.onedrive_tools_enabled)
            if event:
                yield event

    async def truncate(self, item_id, content_index, played_samples):
        # Server VAD interrupt_response cancels generation; cancel again would race response.done.
        await self._send({"type": "conversation.item.truncate", "item_id": item_id,
                          "content_index": content_index,
                          "audio_end_ms": played_samples * 1000 // 16000})

    async def respond(self):
        await self._send({"type": "response.create"})

    async def tool_output(self, call_id, output):
        await self._send({"type": "conversation.item.create",
                          "item": {"type": "function_call_output", "call_id": call_id,
                                   "output": output}})

    async def close(self):
        try:
            async with asyncio.timeout(5):
                try:
                    if self.socket is not None:
                        await self.socket.close()
                finally:
                    await self.credential.close()
        except (AzureError, WebSocketException, OSError, TimeoutError):
            raise ProviderError() from None
