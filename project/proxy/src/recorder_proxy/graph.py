from urllib.parse import quote, urlsplit
import logging
import re

import httpx

from .intelligence_errors import IntelligenceError, check_response, http_client, json_body, unavailable

GRAPH = "https://graph.microsoft.com/v1.0"
FIELDS = ("id,name,size,eTag,cTag,file,folder,parentReference,remoteItem,"
          "lastModifiedDateTime,createdDateTime,package")
logger = logging.getLogger("recorder_proxy.graph")


def identifier(value):
    if (not isinstance(value, str) or not 1 <= len(value) <= 256
            or any(not (c.isascii() and (c.isalnum() or c in "!_-.")) for c in value)
            or value in (".", "..")):
        raise IntelligenceError("invalid_request", 400)
    return value


def clean_item(item, drive=None):
    if (not isinstance(item, dict) or any(k in item for k in ("remoteItem", "symlink",
                                                           "symbolicLink", "package"))
            or "id" not in item):
        raise IntelligenceError("item_not_allowed", 403)
    identifier(item["id"])
    if any(key in item and not isinstance(item[key], dict) for key in ("file", "folder")):
        raise IntelligenceError("item_not_allowed", 403)
    if "file" in item and "folder" in item:
        raise IntelligenceError("item_not_allowed", 403)
    parent = item.get("parentReference", {})
    if not isinstance(parent, dict) or (drive and parent.get("driveId", drive) != drive):
        raise IntelligenceError("item_not_allowed", 403)
    name = item.get("name")
    if (not isinstance(name, str) or not 1 <= len(name) <= 255
            or any(ord(c) < 32 or c in "/\\" for c in name) or name in (".", "..")):
        raise IntelligenceError("item_not_allowed", 403)
    return item


def download_url(url):
    host = ""
    try:
        parsed = urlsplit(url)
        host = parsed.hostname or ""
        allowed = (parsed.scheme == "https" and not parsed.username and not parsed.password
                   and parsed.port in (None, 443) and not parsed.fragment
                   and host.endswith((".files.1drv.com", ".storage.live.com")))
    except ValueError:
        allowed = False
    if not allowed:
        safe_host = host if re.fullmatch(r"[a-z0-9.-]{1,253}", host) else "invalid"
        logger.warning("onedrive_download_origin_rejected host=%s", safe_host)
        raise IntelligenceError("item_not_allowed", 403)
    return url


class GraphClient:
    """Delegated personal-drive access, authorized by ancestry rather than path prefixes."""

    def __init__(self, settings, tokens, user, *, http=None):
        self.settings, self.tokens, self.user = settings, tokens, user
        self.http = http or http_client()
        self.owns_http = http is None
        self.root_id = None
        self.drive_id = None

    async def request(self, method, path, *, content=None, headers=None, missing=False):
        if not path.startswith("/") or path.startswith("//"):
            raise IntelligenceError("item_not_allowed", 403)
        try:
            for attempt in range(2):
                token = await self.tokens.token(self.user, refresh=attempt == 1)
                auth = {"Authorization": f"Bearer {token}", **(headers or {})}
                async with self.http.stream(method, GRAPH + path, headers=auth,
                                            content=content) as response:
                    if response.status_code == 401 and attempt == 0:
                        continue
                    if missing and response.status_code == 404:
                        return None
                    check_response(response)
                    return await json_body(response)
        except httpx.HTTPError:
            raise unavailable() from None
        raise IntelligenceError("consent_required", 403)

    async def root(self):
        if self.root_id:
            return self.drive_id, self.root_id
        drive = await self.request("GET", "/me/drive?$select=id,driveType")
        if drive.get("driveType") != "personal":
            raise IntelligenceError("item_not_allowed", 403)
        drive_id = identifier(drive.get("id"))
        # Resolve each configured folder independently; never traverse remote shortcuts.
        item = await self.request("GET", f"/drives/{drive_id}/root?$select={FIELDS}")
        clean_item(item, drive_id)
        for part in self.settings.onedrive_allowed_root.split("/"):
            item = await self.request(
                "GET", f"/drives/{drive_id}/items/{identifier(item['id'])}:"
                f"/{quote(part, safe='')}?$select={FIELDS}")
            clean_item(item, drive_id)
            if "folder" not in item:
                raise IntelligenceError("item_not_allowed", 403)
        self.drive_id, self.root_id = drive_id, item["id"]
        return drive_id, item["id"]

    async def item(self, drive, item_id):
        drive, item_id = identifier(drive), identifier(item_id)
        allowed_drive, root = await self.root()
        if drive != allowed_drive:
            raise IntelligenceError("item_not_allowed", 403)
        item = await self.request("GET", f"/drives/{drive}/items/{item_id}?$select={FIELDS}")
        original = clean_item(item, drive)
        visited = set()
        for _ in range(32):
            if item["id"] == root:
                return original
            if item["id"] in visited:
                break
            visited.add(item["id"])
            parent = item.get("parentReference", {})
            parent_id = parent.get("id")
            if not parent_id:
                break
            item = await self.request(
                "GET", f"/drives/{drive}/items/{identifier(parent_id)}?$select={FIELDS}")
            clean_item(item, drive)
            if "folder" not in item:
                break
        raise IntelligenceError("item_not_allowed", 403)

    async def children(self, drive, parent, *, limit=50):
        item = await self.item(drive, parent)
        if "folder" not in item:
            raise IntelligenceError("item_not_allowed", 403)
        base = f"/drives/{drive}/items/{parent}/children"
        path = base + f"?$top=10&$select={FIELDS}"
        result, seen = [], set()
        while path and len(result) < limit:
            if path in seen or len(seen) >= 5:
                raise unavailable()
            seen.add(path)
            page = await self.request("GET", path)
            items = page.get("value")
            if not isinstance(items, list) or len(items) > 10:
                raise unavailable()
            for entry in items[:limit - len(result)]:
                clean_item(entry, drive)
                if entry.get("parentReference", {}).get("id") != parent:
                    raise IntelligenceError("item_not_allowed", 403)
                result.append(entry)
            next_url = page.get("@odata.nextLink")
            path = None
            if next_url:
                if not isinstance(next_url, str) or len(next_url) > 4096:
                    raise unavailable()
                parsed = urlsplit(next_url)
                if (parsed.scheme != "https" or parsed.netloc != "graph.microsoft.com"
                        or parsed.path != "/v1.0" + base or parsed.fragment):
                    raise IntelligenceError("item_not_allowed", 403)
                path = base + "?" + parsed.query
        return result

    async def named_child(self, drive, parent, name):
        await self.item(drive, parent)
        if (not isinstance(name, str) or not 1 <= len(name) <= 255
                or any(c in name for c in "/\\") or name in (".", "..")):
            raise IntelligenceError("item_not_allowed", 403)
        value = await self.request(
            "GET", f"/drives/{drive}/items/{parent}:/{quote(name, safe='')}?$select={FIELDS}",
            missing=True)
        if value is not None:
            clean_item(value, drive)
            if value.get("parentReference", {}).get("id") != parent:
                raise IntelligenceError("item_not_allowed", 403)
        return value

    async def download(self, drive, item_id, limit, sink=None):
        item = await self.item(drive, item_id)
        if "file" not in item or type(item.get("size")) is not int or item["size"] > limit:
            raise IntelligenceError("unsupported_audio" if sink else "item_not_allowed",
                                    415 if sink else 403)
        url = f"{GRAPH}/drives/{drive}/items/{item_id}/content"
        token = await self.tokens.token(self.user)
        headers = {"Authorization": f"Bearer {token}"}
        total, chunks = 0, []
        try:
            for hop in range(4):
                async with self.http.stream("GET", url, headers=headers) as response:
                    if response.status_code in (301, 302, 303, 307, 308):
                        url = download_url(response.headers.get("location", ""))
                        headers = {}  # Preauthenticated URL: NEVER forward the Graph bearer.
                        continue
                    if response.status_code == 401 and hop == 0:
                        token = await self.tokens.token(self.user, refresh=True)
                        headers = {"Authorization": f"Bearer {token}"}
                        continue
                    check_response(response)
                    async for chunk in response.aiter_bytes(chunk_size=65536):
                        total += len(chunk)
                        if total > limit:
                            raise IntelligenceError("recording_too_long" if sink else "item_not_allowed",
                                                    413 if sink else 403)
                        if sink:
                            await sink(chunk)
                        else:
                            chunks.append(chunk)
                    if total != item["size"]:
                        raise IntelligenceError("source_changed", 409)
                    return b"".join(chunks) if not sink else total
        except httpx.HTTPError:
            raise unavailable() from None
        raise unavailable()

    async def put_new(self, drive, parent, name, content, content_type):
        await self.item(drive, parent)
        value = await self.request(
            "PUT", f"/drives/{drive}/items/{parent}:/{quote(name, safe='')}"
            ":/content?@microsoft.graph.conflictBehavior=fail",
            content=content, headers={"Content-Type": content_type, "If-None-Match": "*"})
        clean_item(value, drive)
        if value.get("parentReference", {}).get("id") != parent:
            raise IntelligenceError("item_not_allowed", 403)
        return value

    async def close(self):
        if self.owns_http:
            await self.http.aclose()
