import asyncio
from contextlib import asynccontextmanager
import logging
import traceback

from starlette.applications import Starlette
from starlette.responses import JSONResponse
from starlette.requests import ClientDisconnect
from starlette.routing import Route, WebSocketRoute

from .auth import AuthError, AuthUnavailable, TokenValidator
from .config import Settings
from .cleanup import cancel_tasks, finish_task
from .protocol import SUBPROTOCOL
from .providers import VoiceLive
from .sessions import VoiceSession
from .graph import GraphClient
from .intelligence_errors import IntelligenceError, http_client, unavailable
from .obo import GraphTokens, UserContext
from .processing import RecordingProcessor
from .processing_stream import ProcessingStream, SUBPROTOCOL as PROCESSING_SUBPROTOCOL
from .recording_contract import RecordingRequest
from .speech import FastTranscription
from .tools import ReadTools
from .voice_tools import strict_arguments

logger = logging.getLogger("recorder_proxy.processing")


def create_app(settings=None, *, validator=None, provider_factory=None,
               graph_factory=None, speech=None):
    """Dependency injection is Python-only; deployment has no fake/auth-bypass switch."""
    settings = settings or Settings.from_env()
    validator = validator or TokenValidator(settings)
    provider_factory = provider_factory or VoiceLive
    active = set()
    processing_requests = set()
    accepting = False
    tokens, graph_http = None, None
    if (settings.recording_processing_enabled or settings.onedrive_tools_enabled) and graph_factory is None:
        tokens, graph_http = GraphTokens(settings), http_client()

        def graph_factory(user):
            return GraphClient(settings, tokens, user, http=graph_http)

    if settings.recording_processing_enabled:
        speech = speech or FastTranscription(settings)
        processor = RecordingProcessor(settings, graph_factory, speech)
    else:
        processor = None

    @asynccontextmanager
    async def lifespan(app):
        nonlocal accepting
        try:
            await validator.refresh()
            accepting = True
            yield
        finally:
            accepting = False

            async def shutdown():
                await cancel_tasks((*active, *processing_requests))
                await validator.close()
                if speech:
                    await speech.close()
                if tokens:
                    await tokens.close()
                if graph_http:
                    await graph_http.aclose()

            await finish_task(asyncio.create_task(shutdown()))

    async def health(request):
        return JSONResponse({"status": "ok"})

    async def readiness(request):
        ready = accepting and validator.available
        return JSONResponse({"status": "ready" if ready else "not_ready"}, status_code=200 if ready else 503)

    async def deny(socket, status):
        # A denied upgrade never creates an upstream session and doesn't echo request data.
        await socket.send_denial_response(JSONResponse({"error": "request_denied"}, status_code=status))

    async def voice(socket):
        if not accepting:
            await deny(socket, 503)
            return
        if socket.scope.get("query_string") or SUBPROTOCOL not in socket.scope.get("subprotocols", []):
            await deny(socket, 400)
            return
        authorization = socket.headers.getlist("authorization")
        if len(authorization) != 1:
            await deny(socket, 401)
            return
        try:
            principal = await validator.validate(authorization[0])
        except AuthError:
            await deny(socket, 401)
            return
        except AuthUnavailable:
            await deny(socket, 503)
            return
        if active:
            await deny(socket, 429)
            return
        task = asyncio.current_task()
        active.add(task)  # No await between checking and acquiring the process-local slot.
        try:
            await socket.accept(subprotocol=SUBPROTOCOL)
            provider = provider_factory(settings)
            tools = None
            if settings.onedrive_tools_enabled:
                tools = ReadTools(graph_factory, UserContext(principal, authorization[0][7:]))
            session = VoiceSession(socket, provider, settings, principal, tools=tools)
            await session.run()
        finally:
            active.discard(task)

    async def process(request):
        if len(processing_requests) >= 8:
            return intelligence_error(IntelligenceError("busy", 429, True))
        task = asyncio.current_task()
        processing_requests.add(task)
        try:
            if not accepting or not processor:
                raise unavailable()
            if request.scope.get("query_string"):
                raise IntelligenceError("invalid_request", 400)
            authorization = request.headers.getlist("authorization")
            if len(authorization) != 1:
                raise IntelligenceError("authentication_required", 401)
            try:
                principal = await validator.validate(authorization[0])
            except AuthError:
                raise IntelligenceError("authentication_required", 401) from None
            except AuthUnavailable:
                raise unavailable() from None
            if request.headers.get("content-type", "").split(";")[0].strip().lower() != "application/json":
                raise IntelligenceError("invalid_request", 400)
            body = bytearray()
            async with asyncio.timeout(10):
                async for chunk in request.stream():
                    if len(body) + len(chunk) > 4096:
                        raise IntelligenceError("invalid_request", 400)
                    body.extend(chunk)
            try:
                value = strict_arguments(body.decode("utf-8"))
            except (ValueError, RecursionError):
                raise IntelligenceError("invalid_request", 400) from None
            recording = RecordingRequest.parse(value)
            user = UserContext(principal, authorization[0][7:])
            result = await until_disconnect(
                request, processor.handle(user, recording, status=request.url.path.endswith("/status")))
            return JSONResponse(result)
        except (TimeoutError, ClientDisconnect):
            return intelligence_error(unavailable())
        except IntelligenceError as exc:
            frames = traceback.extract_tb(exc.__traceback__)
            origin = frames[-1]
            logger.warning("recording_request_failed operation=%s code=%s origin=%s:%d via=%s",
                           "status" if request.url.path.endswith("/status") else "process",
                           exc.code, origin.name, origin.lineno,
                           "/".join(f"{frame.name}:{frame.lineno}" for frame in frames[-5:]))
            return intelligence_error(exc)
        finally:
            processing_requests.discard(task)

    async def process_stream(socket):
        if not accepting or not processor:
            await deny(socket, 503)
            return
        if (socket.scope.get("query_string")
                or PROCESSING_SUBPROTOCOL not in socket.scope.get("subprotocols", [])):
            await deny(socket, 400)
            return
        if len(processing_requests) >= 8:
            await deny(socket, 429)
            return
        task = asyncio.current_task()
        processing_requests.add(task)
        try:
            authorization = socket.headers.getlist("authorization")
            if len(authorization) != 1:
                await deny(socket, 401)
                return
            try:
                principal = await validator.validate(authorization[0])
            except AuthError:
                await deny(socket, 401)
                return
            except AuthUnavailable:
                await deny(socket, 503)
                return
            await socket.accept(subprotocol=PROCESSING_SUBPROTOCOL)
            await ProcessingStream(socket, processor,
                                   UserContext(principal, authorization[0][7:])).run()
        finally:
            processing_requests.discard(task)

    return Starlette(routes=[
        Route("/healthz", health),
        Route("/readyz", readiness),
        Route("/v1/recordings/process", process, methods=["POST"]),
        Route("/v1/recordings/status", process, methods=["POST"]),
        WebSocketRoute("/v1/recordings/process-stream", process_stream),
        WebSocketRoute("/v1/voice", voice),
    ], lifespan=lifespan)


def intelligence_error(error):
    return JSONResponse({"v": 1, "error": {"code": error.code, "retryable": error.retryable}},
                        status_code=error.status,
                        headers={"Retry-After": "5"} if error.status in (429, 503, 504) else {})


async def until_disconnect(request, operation):
    async def disconnected():
        while True:
            if (await request.receive())["type"] == "http.disconnect":
                raise ClientDisconnect()

    work, watch = asyncio.create_task(operation), asyncio.create_task(disconnected())
    try:
        done, _ = await asyncio.wait((work, watch), return_when=asyncio.FIRST_COMPLETED)
        if work in done:
            return work.result()
        watch.result()
    finally:
        await cancel_tasks((work, watch))
