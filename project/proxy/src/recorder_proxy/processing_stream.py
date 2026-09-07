import asyncio
import json
import logging
import time
import traceback

from starlette.websockets import WebSocketDisconnect

from .cleanup import cancel_tasks, finish_task
from .intelligence_errors import IntelligenceError, unavailable
from .recording_contract import RecordingRequest
from .voice_tools import strict_arguments

SUBPROTOCOL = "recorder.processing.v1"
MAX_MESSAGE_BYTES = 4096
INITIAL_REQUEST_SECONDS = 15
HEARTBEAT_SECONDS = 5
SEND_SECONDS = 5
PHASES = frozenset(("resolving", "downloading", "validating", "transcribing",
                    "saving", "verifying"))
logger = logging.getLogger("recorder_proxy.processing")


class ProcessingStream:
    """All work and bounded transport tasks belong to this socket, never a worker."""

    def __init__(self, socket, processor, user):
        self.socket, self.processor, self.user = socket, processor, user
        self.phase = "resolving"
        self.updates = asyncio.Queue(maxsize=1)

    async def progress(self, phase, **counts):
        if phase not in PHASES:
            raise unavailable()
        self.phase = phase
        message = {"v": 1, "type": "progress", "phase": phase, **counts}
        if self.updates.full():
            self.updates.get_nowait()
        self.updates.put_nowait(message)
        # Yield to the single writer without waiting for a slow socket or
        # allocating one task per download/upload chunk.
        await asyncio.sleep(0)

    async def send(self, message):
        raw = json.dumps(message, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
        if len(raw.encode("utf-8")) > MAX_MESSAGE_BYTES:
            raise unavailable()
        async with asyncio.timeout(SEND_SECONDS):
            await self.socket.send_text(raw)

    async def receive(self):
        message = await self.socket.receive()
        if message["type"] == "websocket.disconnect":
            raise WebSocketDisconnect(message.get("code", 1000))
        raw = message.get("text")
        try:
            # ASGI exposes whole, reassembled messages; uvicorn must also retain
            # its 4096-byte transport bound to cap fragmented-message allocation.
            if not isinstance(raw, str) or len(raw.encode("utf-8")) > MAX_MESSAGE_BYTES:
                raise ValueError()
            return strict_arguments(raw)
        except (ValueError, RecursionError):
            raise IntelligenceError("invalid_request", 400) from None

    async def watch_client(self):
        value = await self.receive()
        if value != {"v": 1, "type": "cancel"} or type(value.get("v")) is not int:
            raise IntelligenceError("invalid_request", 400)

    async def writer(self):
        while True:
            try:
                async with asyncio.timeout(HEARTBEAT_SECONDS):
                    message = await self.updates.get()
            except TimeoutError:
                message = {"v": 1, "type": "heartbeat", "phase": self.phase}
            await self.send(message)

    async def serve(self):
        try:
            async with asyncio.timeout(INITIAL_REQUEST_SECONDS):
                value = await self.receive()
        except TimeoutError:
            raise IntelligenceError("invalid_request", 400) from None
        request = RecordingRequest.parse(value)
        await self.send({"v": 1, "type": "started",
                         "operation_id": request.operation(self.user.principal.oid)})
        work = asyncio.create_task(self.processor.handle(
            self.user, request, stream=True, progress=self.progress))
        watch = asyncio.create_task(self.watch_client())
        writer = asyncio.create_task(self.writer())
        tasks = (work, watch, writer)
        try:
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            if watch in done:
                watch.result()
                return None
            if writer in done:
                writer.result()
            return work.result()
        finally:
            await cancel_tasks(tasks)

    async def expire(self):
        await asyncio.sleep(max(0, self.user.principal.expires_at - time.time()))
        raise IntelligenceError("authentication_required", 401)

    async def run(self):
        work, expiry = asyncio.create_task(self.serve()), asyncio.create_task(self.expire())
        result, error = None, None
        disconnected = False
        try:
            try:
                done, _ = await asyncio.wait((work, expiry), return_when=asyncio.FIRST_COMPLETED)
                if expiry in done:
                    expiry.result()
                result = work.result()
            except WebSocketDisconnect:
                disconnected = True
            except IntelligenceError as exc:
                error = exc
                frames = traceback.extract_tb(exc.__traceback__)
                logger.warning("recording_request_failed operation=stream code=%s origin=%s:%d via=%s",
                               exc.code, frames[-1].name, frames[-1].lineno,
                               "/".join(f"{frame.name}:{frame.lineno}" for frame in frames[-5:]))
            except Exception as exc:
                error = unavailable()
                frames = traceback.extract_tb(exc.__traceback__)
                logger.warning("recording_stream_failed kind=%s origin=%s:%d",
                               type(exc).__name__, frames[-1].name, frames[-1].lineno)
            finally:
                await cancel_tasks((work, expiry))
            if not disconnected:
                try:
                    if error:
                        await self.send({"v": 1, "type": "error",
                                         "error": {"code": error.code, "retryable": error.retryable}})
                    elif result:
                        await self.send({**result, "type": "result"})
                        logger.info("recording_stream_finished status=%s", result["status"])
                except (WebSocketDisconnect, OSError, RuntimeError, TimeoutError):
                    pass
        finally:
            async def close():
                try:
                    async with asyncio.timeout(SEND_SECONDS):
                        await self.socket.close(code=1008 if error else 1000)
                except (WebSocketDisconnect, OSError, RuntimeError, TimeoutError):
                    pass

            await finish_task(asyncio.create_task(close()))
