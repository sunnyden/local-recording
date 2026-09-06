import asyncio
import json
import socket
import time

from cryptography.hazmat.primitives.asymmetric import rsa
import pytest
import pytest_asyncio
import uvicorn
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed

from recorder_proxy.app import create_app
from recorder_proxy.config import CONSUMER_TENANT, ISSUER
from recorder_proxy.protocol import Frame
from test_auth import token, validator
from test_sessions import FakeProvider, HELLO


@pytest_asyncio.fixture
async def server(settings):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    now = int(time.time())
    claims = {"iss": ISSUER, "aud": settings.api_audience, "tid": CONSUMER_TENANT,
              "oid": settings.allowed_oid, "azp": settings.client_id, "scp": "access_as_user",
              "ver": "2.0", "iat": now - 1, "nbf": now - 1, "exp": now + 30}
    auth, _ = validator(settings, key)
    provider = FakeProvider()
    app = create_app(settings, validator=auth, provider_factory=lambda _: provider)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]
    config = uvicorn.Config(
        app, ws="websockets", ws_max_size=4096, ws_max_queue=4,
        ws_per_message_deflate=False, ws_ping_interval=0.1, ws_ping_timeout=0.2,
        access_log=False, log_level="critical", timeout_graceful_shutdown=2,
    )
    running = uvicorn.Server(config)
    task = asyncio.create_task(running.serve(sockets=[listener]))
    try:
        async with asyncio.timeout(3):
            while not running.started:
                if task.done():
                    task.result()
                await asyncio.sleep(0.01)
        yield f"ws://127.0.0.1:{port}/v1/voice", token(key, claims), provider
    finally:
        running.should_exit = True
        await asyncio.wait_for(task, 3)
        listener.close()
        assert provider.closed


async def ready(client):
    while json.loads(await client.recv())["type"] != "ready":
        pass


async def test_real_websocket_fragment_reassembly_and_ping(server):
    url, authorization, provider = server
    async with connect(url, additional_headers={"Authorization": authorization},
                       subprotocols=["recorder.voice.v1"], proxy=None) as client:
        hello = json.dumps(HELLO)
        await client.send([hello[:5], hello[5:]])
        await ready(client)
        wire = Frame(1, 0, 0, 0, bytes(640)).encode()
        await client.send([wire[:3], wire[3:24], wire[24:]])
        async with asyncio.timeout(2):
            while provider.appended != [bytes(640)]:
                await asyncio.sleep(0.01)
        pong = await client.ping()
        await asyncio.wait_for(pong, 1)
        await client.send(Frame(1, 0, 2, 320, bytes(640)).encode())
        while True:
            message = json.loads(await client.recv())
            if message["type"] == "error":
                assert message["code"] == "protocol_error"
                break


async def test_real_websocket_total_fragment_size_is_bounded(server):
    url, authorization, provider = server
    async with connect(url, additional_headers={"Authorization": authorization},
                       subprotocols=["recorder.voice.v1"], proxy=None) as client:
        await client.send(json.dumps(HELLO))
        await ready(client)
        await client.send(["x" * 2048, "x" * 2049])
        with pytest.raises(ConnectionClosed) as exc:
            while True:
                await client.recv()
        assert exc.value.rcvd.code == 1009
