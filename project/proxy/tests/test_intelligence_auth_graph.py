from dataclasses import replace
import time
from urllib.parse import parse_qs

import httpx
import pytest

from recorder_proxy.graph import GraphClient, download_url, identifier
from recorder_proxy.intelligence_errors import IntelligenceError
from recorder_proxy.obo import AUTHORITY, EXCHANGE_SCOPE, GRAPH_SCOPE, GraphTokens, UserContext
from intelligence_fakes import Credential, Drive, Tokens


async def test_obo_exact_confidential_assertions_cache_and_renewal(settings, principal):
    credential, calls = Credential(), []

    async def handle(request):
        calls.append(request)
        assert str(request.url) == AUTHORITY
        body = parse_qs((await request.aread()).decode())
        assert body == {
            "client_id": [settings.api_audience],
            "client_assertion_type": ["urn:ietf:params:oauth:client-assertion-type:jwt-bearer"],
            "client_assertion": [f"managed-assertion-{len(calls)}"],
            "grant_type": ["urn:ietf:params:oauth:grant-type:jwt-bearer"],
            "requested_token_use": ["on_behalf_of"], "assertion": ["validated-api-b"],
            "scope": [GRAPH_SCOPE],
        }
        return httpx.Response(200, json={"access_token": f"graph-{len(calls)}",
                                        "expires_in": 3600, "token_type": "Bearer"})

    principal = replace(principal, expires_at=int(time.time()) + 3600)
    tokens = GraphTokens(settings, http=httpx.AsyncClient(transport=httpx.MockTransport(handle)),
                         credential=credential)
    user = UserContext(principal, "validated-api-b")
    assert "validated-api-b" not in repr(user)
    assert await tokens.token(user) == "graph-1"
    assert await tokens.token(user) == "graph-1"
    assert await tokens.token(user, refresh=True) == "graph-2"
    key = next(iter(tokens.cache))
    tokens.cache[key] = ("expired", 0)
    assert await tokens.token(user) == "graph-3"
    assert credential.scopes == [EXCHANGE_SCOPE] * 3
    await tokens.close()
    assert not tokens.cache and credential.closed


async def test_obo_user_binding_expiry_and_bounded_cache(settings, principal):
    calls = []

    async def handle(request):
        calls.append(request)
        return httpx.Response(200, json={"access_token": "graph", "expires_in": 3600,
                                        "token_type": "Bearer"})

    tokens = GraphTokens(settings, credential=Credential(),
                         http=httpx.AsyncClient(transport=httpx.MockTransport(handle)))
    for current in (replace(principal, oid="another-user"),
                    replace(principal, expires_at=int(time.time()) - 1)):
        with pytest.raises(IntelligenceError, match="authentication_required"):
            await tokens.token(UserContext(current, "B"))
    assert not calls
    for i in range(12):
        await tokens.token(UserContext(principal, f"B-{i}"))
    assert len(tokens.cache) == 8
    assert all(not isinstance(key[1], str) for key in tokens.cache)
    await tokens.close()


@pytest.mark.parametrize("status,body,error", [
    (400, {"error": "invalid_grant", "error_description": "secret"}, "consent_required"),
    (401, {}, "consent_required"), (429, {}, "temporarily_unavailable"),
    (200, {"access_token": "token", "expires_in": True}, "temporarily_unavailable"),
    (200, {"access_token": "token", "expires_in": 3600, "token_type": "MAC"}, "temporarily_unavailable"),
])
async def test_obo_errors_redacted(settings, principal, status, body, error):
    tokens = GraphTokens(settings, credential=Credential(),
                         http=httpx.AsyncClient(transport=httpx.MockTransport(
                             lambda _: httpx.Response(status, json=body))))
    with pytest.raises(IntelligenceError, match=error) as exc:
        await tokens.token(UserContext(principal, "B"))
    assert "secret" not in str(exc.value)
    await tokens.close()


@pytest.fixture
async def graph(settings, principal):
    drive, tokens = Drive(), Tokens()
    http = httpx.AsyncClient(transport=httpx.MockTransport(drive.handle))
    graph = GraphClient(settings, tokens, UserContext(principal, "B"), http=http)
    yield graph, drive, tokens
    await http.aclose()


@pytest.mark.parametrize("redirect", [
    "https://unit.files.1drv.com/download?preauthenticated=yes",
    "https://my.microsoftpersonalcontent.com/personal/fixture/download?tempauth=TEST_ONLY",
])
async def test_scope_ancestry_and_download_no_cross_host_bearer(graph, redirect):
    client, drive, _ = graph
    assert (await client.item("drive", "audio"))["id"] == "audio"
    drive.redirect = redirect
    assert await client.download("drive", "audio", 4096) == drive.content["audio"]
    assert "authorization" not in drive.calls[-1].headers
    drive.add("outside", "local-recording-spoof", "root", folder=True)
    drive.items["audio"]["parentReference"]["id"] = "outside"
    with pytest.raises(IntelligenceError, match="item_not_allowed"):
        await client.item("drive", "audio")


@pytest.mark.parametrize("url", [
    "http://unit.files.1drv.com/a", "https://evil.test/a",
    "https://unit.files.1drv.com.evil.test/a", "https://u:p@unit.files.1drv.com/a",
    "https://unit.files.1drv.com:444/a", "https://127.0.0.1/a",
    "https://graph.microsoft.com/v1.0/me", "//unit.files.1drv.com/a",
    "file:///private.wav", "https://unit.files.1drv.com/a#fragment",
    "https://my.microsoftpersonalcontent.com.evil.test/a",
    "https://evilmy.microsoftpersonalcontent.com/a",
    "https://other.microsoftpersonalcontent.com/a",
    "http://my.microsoftpersonalcontent.com/a",
    "https://my.microsoftpersonalcontent.com:444/a",
    "https://user:password@my.microsoftpersonalcontent.com/a",
])
def test_redirect_allowlist(url):
    with pytest.raises(IntelligenceError):
        download_url(url)


def test_redirect_rejection_logs_only_host(caplog):
    with pytest.raises(IntelligenceError):
        download_url("https://new.microsoft-host.example/download/PRIVATE_PATH?token=PRIVATE_CAPABILITY")
    messages = [record.getMessage() for record in caplog.records
                if record.name == "recorder_proxy.graph"]
    assert messages == ["onedrive_download_origin_rejected host=new.microsoft-host.example"]
    assert "PRIVATE" not in messages[0]


@pytest.mark.parametrize("value", ["a/b", "..", "https://evil", "", 1, "a?b", "a%2fb", "a\\b"])
def test_ids_are_not_urls_or_paths(value):
    with pytest.raises(IntelligenceError):
        identifier(value)


@pytest.mark.parametrize("facet", ["remoteItem", "symlink", "symbolicLink", "package"])
async def test_remote_shortcuts_rejected_at_source_and_ancestor(graph, facet):
    client, drive, _ = graph
    drive.items["folder"][facet] = {}
    with pytest.raises(IntelligenceError, match="item_not_allowed"):
        await client.item("drive", "audio")


async def test_cross_drive_parent_and_cycle_rejected(graph):
    client, drive, _ = graph
    with pytest.raises(IntelligenceError, match="item_not_allowed"):
        await client.item("other-drive", "audio")
    drive.items["audio"]["parentReference"]["driveId"] = "other-drive"
    with pytest.raises(IntelligenceError, match="item_not_allowed"):
        await client.item("drive", "audio")
    drive.items["audio"]["parentReference"]["driveId"] = "drive"
    drive.add("cycle", "cycle", "cycle", folder=True)
    drive.items["audio"]["parentReference"]["id"] = "cycle"
    with pytest.raises(IntelligenceError, match="item_not_allowed"):
        await client.item("drive", "audio")


@pytest.mark.parametrize("next_link", [
    "https://evil.test/next", "https://graph.microsoft.com/v1.0/me/messages",
    "https://graph.microsoft.com/v1.0/drives/drive/items/root/children",
])
async def test_pagination_cannot_escape_authorized_collection(graph, next_link):
    client, drive, _ = graph
    drive.next_link = next_link
    with pytest.raises(IntelligenceError, match="item_not_allowed"):
        await client.children("drive", "folder")
    assert all(request.url.host == "graph.microsoft.com" for request in drive.calls)


async def test_graph_401_refreshes_once(settings, principal):
    drive, tokens, count = Drive(), Tokens(), 0

    async def handler(request):
        nonlocal count
        count += 1
        if count == 1:
            return httpx.Response(401)
        return await drive.handle(request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        client = GraphClient(settings, tokens, UserContext(principal, "B"), http=http)
        assert await client.root() == ("drive", "folder")
        assert tokens.refreshes[:2] == [False, True]


async def test_pagination_cycle_and_response_size_are_bounded(graph):
    client, drive, _ = graph
    drive.next_link = ("https://graph.microsoft.com/v1.0/drives/drive/items/folder/children"
                       "?$skiptoken=repeat")
    with pytest.raises(IntelligenceError, match="temporarily_unavailable"):
        await client.children("drive", "folder")
    assert len(drive.calls) < 20


async def test_streamed_download_limit_rejects_larger_actual_body(graph):
    client, drive, _ = graph
    drive.items["audio"]["size"] = 1
    with pytest.raises(IntelligenceError, match="item_not_allowed"):
        await client.download("drive", "audio", 100)


def test_intelligence_config_is_explicit_and_off_by_default():
    from recorder_proxy.config import Settings
    from test_auth import env

    config = Settings.from_env(env())
    assert not config.recording_processing_enabled and not config.onedrive_tools_enabled
    assert config.onedrive_allowed_root == "local-recording"
    enabled = {**env(), "RECORDING_PROCESSING_ENABLED": "true",
               "ONEDRIVE_TOOLS_ENABLED": "true",
               "SPEECH_ENDPOINT": "https://existing.cognitiveservices.azure.com"}
    assert Settings.from_env(enabled).recording_processing_enabled
    assert Settings.from_env({**enabled, "GRAPH_ROOT_PATH": "archive/recordings"}).onedrive_allowed_root == "archive/recordings"
    assert Settings.from_env({**enabled, "ONEDRIVE_ALLOWED_ROOT": "archive"}).onedrive_allowed_root == "archive"
    for changes in (
        {"SPEECH_ENDPOINT": "http://existing.cognitiveservices.azure.com"},
        {"SPEECH_ENDPOINT": "https://existing.cognitiveservices.azure.com.evil.test"},
        {"SPEECH_ENDPOINT": "https://existing.cognitiveservices.azure.com?secret=1"},
        {"SPEECH_ENDPOINT": ""}, {"ONEDRIVE_TOOLS_ENABLED": "yes"},
        {"GRAPH_ROOT_PATH": "../local-recording"},
        {"GRAPH_ROOT_PATH": "local-recording//sub"},
        {"GRAPH_ROOT_PATH": "/local-recording"},
        {"GRAPH_ROOT_PATH": "recordings", "ONEDRIVE_ALLOWED_ROOT": "different"},
    ):
        with pytest.raises(ValueError):
            Settings.from_env({**enabled, **changes})


async def test_folder_content_search_escapes_odata_and_validates_hits(settings, principal):
    drive = Drive()
    requests = []

    async def handler(request):
        if "/search(" in request.url.path:
            requests.append(request)
            return httpx.Response(200, json={"value": [drive.items["audio"]]})
        return await drive.handle(request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        client = GraphClient(settings, Tokens(), UserContext(principal, "B"), http=http)
        client.enable_read_cache()
        items = await client.search("drive", "folder", "budget's / plan? #next", limit=10)
        assert [item["id"] for item in items] == ["audio"]
        assert requests[0].url.path.startswith("/v1.0/drives/drive/items/folder/search(q=")
        assert "budget''s" in requests[0].url.path
        assert b"%2F" in requests[0].url.raw_path and b"%3F" in requests[0].url.raw_path
        assert requests[0].url.params["$top"] == "10"
        drive.items["audio"]["parentReference"]["id"] = "root"
        fresh = GraphClient(settings, Tokens(), UserContext(principal, "B"), http=http)
        fresh.enable_read_cache()
        assert await fresh.search("drive", "folder", "budget") == []


@pytest.mark.parametrize("projection", [
    {"id": "audio"},
    {"id": "audio", "name": "index-name", "file": {}, "folder": None,
     "remoteItem": None, "package": None},
    {"id": "audio", "name": "index-name", "parentReference": {"driveId": "INDEX-DRIVE"}},
])
async def test_search_uses_canonical_metadata_not_index_projection(settings, principal, projection):
    drive = Drive()

    async def handler(request):
        if "/search(" in request.url.path:
            return httpx.Response(200, json={"value": [projection]})
        return await drive.handle(request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        client = GraphClient(settings, Tokens(), UserContext(principal, "B"), http=http)
        result = await client.search("drive", "folder", "recording")
        assert result == [drive.items["audio"]]
        assert result[0]["name"] != projection.get("name")


@pytest.mark.parametrize("change", [
    {"remoteItem": {}},
    {"parentReference": {"driveId": "other-drive", "id": "folder"}},
    {"parentReference": {"driveId": "drive", "id": "root"}},
])
async def test_search_projection_cannot_override_authoritative_scope(settings, principal, change):
    drive = Drive()
    drive.items["audio"].update(change)

    async def handler(request):
        if "/search(" in request.url.path:
            return httpx.Response(200, json={"value": [{"id": "audio"}]})
        return await drive.handle(request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        client = GraphClient(settings, Tokens(), UserContext(principal, "B"), http=http)
        assert await client.search("drive", "folder", "recording") == []


async def test_search_filters_outside_and_deleted_hits_without_losing_valid_results(settings, principal):
    drive = Drive()
    drive.add("outside", "private-outside.txt", "root", b"must not be returned")

    async def handler(request):
        if "/search(" in request.url.path:
            return httpx.Response(200, json={"value": [
                {"id": "outside"}, {"id": "deleted"}, {"id": "audio"},
            ]})
        return await drive.handle(request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        client = GraphClient(settings, Tokens(), UserContext(principal, "B"), http=http)
        items = await client.search("drive", "folder", "recording")
        assert [item["id"] for item in items] == ["audio"]
        assert not any(request.url.path.endswith("/content") for request in drive.calls)


async def test_search_does_not_hide_graph_authorization_failures(settings, principal):
    drive = Drive()

    async def handler(request):
        if "/search(" in request.url.path:
            return httpx.Response(200, json={"value": [{"id": "audio"}]})
        if request.url.path.endswith("/items/audio"):
            return httpx.Response(403)
        return await drive.handle(request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        client = GraphClient(settings, Tokens(), UserContext(principal, "B"), http=http)
        with pytest.raises(IntelligenceError, match="consent_required"):
            await client.search("drive", "folder", "recording")


async def test_search_next_page_cannot_change_search_root_or_query(settings, principal):
    drive = Drive()

    async def handler(request):
        if "/search(" in request.url.path:
            return httpx.Response(200, json={"value": [], "@odata.nextLink":
                "https://graph.microsoft.com/v1.0/drives/drive/root/search(q='secret')?$skiptoken=x"})
        return await drive.handle(request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        client = GraphClient(settings, Tokens(), UserContext(principal, "B"), http=http)
        with pytest.raises(IntelligenceError, match="item_not_allowed"):
            await client.search("drive", "folder", "budget")


async def test_read_request_cache_avoids_repeated_ancestry_without_caching_processing(graph):
    client, drive, _ = graph
    client.enable_read_cache()
    await client.item("drive", "audio")
    before = len(drive.calls)
    await client.item("drive", "audio")
    assert len(drive.calls) == before
    assert await client.download("drive", "audio", 4096) == drive.content["audio"]
    assert len(drive.calls) == before + 1
    fresh = GraphClient(client.settings, client.tokens, client.user, http=client.http)
    await fresh.item("drive", "audio")
    drive.items["audio"]["eTag"] = '"new-version"'
    assert (await fresh.item("drive", "audio"))["eTag"] == '"new-version"'


async def test_search_subfolder_does_not_accept_sibling_hit(settings, principal):
    drive = Drive()
    drive.add("sub", "nested", "folder", folder=True)

    async def handler(request):
        if "/search(" in request.url.path:
            return httpx.Response(200, json={"value": [drive.items["audio"]]})
        return await drive.handle(request)

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as http:
        client = GraphClient(settings, Tokens(), UserContext(principal, "B"), http=http)
        assert await client.search("drive", "sub", "budget") == []
