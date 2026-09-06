import asyncio
from contextlib import asynccontextmanager

import anyio
from starlette.applications import Starlette
from starlette.responses import JSONResponse
from starlette.routing import Route, WebSocketRoute

from .auth import AuthError, AuthUnavailable, TokenValidator
from .config import Settings
from .protocol import SUBPROTOCOL
from .providers import VoiceLive
from .sessions import VoiceSession


def create_app(settings=None, *, validator=None, provider_factory=None):
    """Dependency injection is Python-only; deployment has no fake/auth-bypass switch."""
    settings = settings or Settings.from_env()
    validator = validator or TokenValidator(settings)
    provider_factory = provider_factory or VoiceLive
    active = set()
    accepting = False

    @asynccontextmanager
    async def lifespan(app):
        nonlocal accepting
        try:
            await validator.refresh()
            accepting = True
            yield
        finally:
            accepting = False
            with anyio.CancelScope(shield=True):
                for task in list(active):
                    task.cancel()
                await asyncio.gather(*active, return_exceptions=True)
                await validator.close()

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
            session = VoiceSession(socket, provider, settings, principal)
            await session.run()
        finally:
            active.discard(task)

    return Starlette(routes=[
        Route("/healthz", health),
        Route("/readyz", readiness),
        WebSocketRoute("/v1/voice", voice),
    ], lifespan=lifespan)
