import asyncio
import json
from pathlib import Path
import threading
from types import SimpleNamespace

import pytest

from recorder_proxy import processing as processing_module, processing_stream
from recorder_proxy.app import create_app, until_disconnect
from recorder_proxy.speech import disk
from test_processing import processing
from test_processing_stream import ProcessingStream, Socket, request_body


async def eventually(predicate):
    async with asyncio.timeout(2):
        while not predicate():
            await asyncio.sleep(0.001)


class DelayedOpen:
    def __init__(self, monkeypatch):
        self.entered, self.release = asyncio.Event(), threading.Event()
        self.files, self.paths = [], []
        loop = asyncio.get_running_loop()
        original = processing_module.os

        def open(path, *args):
            self.paths.append(Path(path))
            loop.call_soon_threadsafe(self.entered.set)
            if not self.release.wait(5):
                raise RuntimeError("Test did not release filesystem operation")
            return original.open(path, *args)

        def fdopen(*args):
            file = original.fdopen(*args)
            self.files.append(file)
            return file

        namespace = SimpleNamespace(**vars(original))
        namespace.open, namespace.fdopen = open, fdopen
        monkeypatch.setattr(processing_module, "os", namespace)

    def assert_clean(self):
        assert self.files and all(file.closed for file in self.files)
        assert self.paths and all(not path.exists() for path in self.paths)

    def cleanup(self):
        for file in self.files:
            if not file.closed:
                file.close()
        for path in self.paths:
            path.unlink(missing_ok=True)


@pytest.mark.parametrize("command", ["cancel", "disconnect"])
async def test_client_cancel_then_auth_expiry_waits_for_pending_spool_open(
        processing, monkeypatch, caplog, command):
    processor, request, user, drive, service, _ = processing
    gate = DelayedOpen(monkeypatch)
    monkeypatch.setattr(processing_stream, "time",
                        SimpleNamespace(time=lambda: user.principal.expires_at - 0.1))
    original = processor.handle
    owned = None

    async def handle(*args, **kwargs):
        nonlocal owned
        owned = asyncio.current_task()
        return await original(*args, **kwargs)

    processor.handle = handle
    socket = Socket(request_body(request))
    baseline = asyncio.all_tasks()
    task = asyncio.create_task(ProcessingStream(socket, processor, user).run())
    try:
        await asyncio.wait_for(gate.entered.wait(), 2)
        socket.incoming.put_nowait(
            {"type": "websocket.disconnect", "code": 1000} if command == "disconnect" else
            {"type": "websocket.receive", "text": '{"v":1,"type":"cancel"}'})
        await eventually(lambda: owned.cancelling() > 0)
        await eventually(lambda: "code=authentication_required" in caplog.text)
        await asyncio.sleep(0.02)
        assert not task.done() and not owned.done()
        assert owned.cancelling() == 1
        assert processor.running == request.operation(user.principal.oid)
        assert not gate.files
        gate.release.set()
        await asyncio.wait_for(task, 2)
        gate.assert_clean()
        assert socket.closed and processor.running is None
        assert not service.calls and not drive.writes
        assert not (asyncio.all_tasks() - baseline)
    finally:
        gate.release.set()
        await asyncio.gather(task, return_exceptions=True)
        gate.cleanup()


@pytest.mark.parametrize("operation", ["write", "close", "failure"])
async def test_disk_joins_thread_through_repeated_cancellation(operation):
    path = Path(".processing-cleanup-test.wav")
    file = path.open("w+b")
    entered, release = asyncio.Event(), threading.Event()
    loop = asyncio.get_running_loop()
    finished = False

    def work():
        nonlocal finished
        loop.call_soon_threadsafe(entered.set)
        if not release.wait(5):
            raise RuntimeError("Test did not release filesystem operation")
        try:
            if operation == "write":
                file.write(b"test")
                file.flush()
            elif operation == "close":
                file.close()
            else:
                raise OSError("filesystem failure")
        finally:
            finished = True

    task = asyncio.create_task(disk(work))
    try:
        await asyncio.wait_for(entered.wait(), 2)
        for index in range(3):
            task.cancel(f"cancel-{index}")
            await asyncio.sleep(0)
        assert not task.done() and not finished
        release.set()
        with pytest.raises(asyncio.CancelledError) as exc:
            await asyncio.wait_for(task, 2)
        assert exc.value.args == ("cancel-0",)
        assert finished
        if operation == "write":
            assert path.read_bytes() == b"test"
        elif operation == "close":
            assert file.closed
    finally:
        release.set()
        await asyncio.gather(task, return_exceptions=True)
        file.close()
        path.unlink(missing_ok=True)


async def test_disk_preserves_uncancelled_filesystem_error():
    def work():
        raise OSError("filesystem failure")

    with pytest.raises(OSError, match="filesystem failure"):
        await disk(work)


async def test_http_disconnect_then_repeated_parent_cancel_waits_for_spool(processing, monkeypatch):
    processor, request, user, drive, service, _ = processing
    gate = DelayedOpen(monkeypatch)
    disconnected = asyncio.Event()
    owned = None

    async def work():
        nonlocal owned
        owned = asyncio.current_task()
        return await processor.handle(user, request)

    class Request:
        async def receive(self):
            await disconnected.wait()
            return {"type": "http.disconnect"}

    baseline = asyncio.all_tasks()
    task = asyncio.create_task(until_disconnect(Request(), work()))
    try:
        await asyncio.wait_for(gate.entered.wait(), 2)
        disconnected.set()
        await eventually(lambda: owned.cancelling() > 0)
        for _ in range(3):
            task.cancel()
            await asyncio.sleep(0)
        assert not task.done() and not owned.done()
        assert owned.cancelling() == 1 and processor.running is not None
        gate.release.set()
        with pytest.raises(asyncio.CancelledError):
            await asyncio.wait_for(task, 2)
        gate.assert_clean()
        assert processor.running is None and not drive.writes and not service.calls
        assert not (asyncio.all_tasks() - baseline)
    finally:
        gate.release.set()
        await asyncio.gather(task, return_exceptions=True)
        gate.cleanup()


async def test_lifespan_repeated_cancel_waits_for_http_and_closes_resources(processing, monkeypatch):
    processor, request, user, drive, service, credential = processing
    gate = DelayedOpen(monkeypatch)
    ready, shutdown = asyncio.Event(), asyncio.Event()

    class Validator:
        available = True
        closed = False

        async def refresh(self):
            pass

        async def validate(self, authorization):
            return user.principal

        async def close(self):
            self.closed = True

    validator = Validator()
    app = create_app(processor.settings, validator=validator,
                     graph_factory=processor.graph_factory, speech=processor.speech)

    async def lifespan():
        async with app.router.lifespan_context(app):
            ready.set()
            await shutdown.wait()

    body = json.dumps(request_body(request)).encode()
    sent = False

    async def receive():
        nonlocal sent
        if not sent:
            sent = True
            return {"type": "http.request", "body": body, "more_body": False}
        await asyncio.Event().wait()

    async def send(message):
        pass

    scope = {
        "type": "http", "asgi": {"version": "3.0"}, "http_version": "1.1",
        "method": "POST", "scheme": "http", "path": "/v1/recordings/process",
        "query_string": b"", "root_path": "",
        "headers": [(b"authorization", b"Bearer fixture"), (b"content-type", b"application/json")],
        "server": ("fixture", 80), "client": ("fixture", 1),
    }
    baseline = asyncio.all_tasks()
    life = asyncio.create_task(lifespan())
    await ready.wait()
    http = asyncio.create_task(app(scope, receive, send))
    try:
        await asyncio.wait_for(gate.entered.wait(), 2)
        shutdown.set()
        await eventually(lambda: http.cancelling() > 0)
        for _ in range(3):
            life.cancel()
            await asyncio.sleep(0)
        assert not life.done() and not http.done()
        assert http.cancelling() == 1 and not validator.closed
        gate.release.set()
        with pytest.raises(asyncio.CancelledError):
            await asyncio.wait_for(life, 2)
        assert http.cancelled()
        gate.assert_clean()
        assert validator.closed and credential.closed
        assert not service.calls and not drive.writes
        assert not (asyncio.all_tasks() - baseline)
    finally:
        gate.release.set()
        shutdown.set()
        await asyncio.gather(life, http, return_exceptions=True)
        gate.cleanup()
