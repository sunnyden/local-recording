import hashlib
import json
import re

from .intelligence_errors import IntelligenceError
from .recording_contract import PIPELINE

MAX_SIDECAR = 4 * 1024 * 1024


def conflict():
    return IntelligenceError("output_conflict", 409)


def text_bytes(operation, request, phrases):
    lines = [f"[{p['offset_ms']}ms +{p['duration_ms']}ms {p['locale']}] {p['text']}" for p in phrases]
    body = ("\n".join(lines or ["(No speech recognized.)"]) + "\n").encode("utf-8")
    header = (f"Recorder transcription v1\nOperation: {operation}\nSource SHA1: {request.source_sha1}\n"
              f"Content SHA256: {hashlib.sha256(body).hexdigest()}\n\n")
    return header.encode() + body


def recover_text(raw, operation, request):
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise conflict() from None
    prefix = (f"Recorder transcription v1\nOperation: {operation}\n"
              f"Source SHA1: {request.source_sha1}\nContent SHA256: ")
    if not text.startswith(prefix) or not text.endswith("\n"):
        raise conflict()
    rest = text[len(prefix):]
    checksum, separator, content = rest.partition("\n\n")
    if (not separator or not re.fullmatch("[0-9a-f]{64}", checksum)
            or hashlib.sha256(content.encode()).hexdigest() != checksum):
        raise conflict()
    body = content[:-1]
    phrases = []
    if body != "(No speech recognized.)":
        for line in body.split("\n"):
            match = re.fullmatch(r"\[(\d{1,7})ms \+(\d{1,7})ms ([A-Za-z0-9-]{1,35})\] (.+)", line)
            if not match:
                raise conflict()
            offset, duration = int(match[1]), int(match[2])
            if offset + duration > 1801000:
                raise conflict()
            phrases.append({"offset_ms": offset, "duration_ms": duration,
                            "locale": match[3], "text": match[4]})
        if len(phrases) > 20000:
            raise conflict()
    if text_bytes(operation, request, phrases) != raw:
        raise conflict()
    return phrases


def json_bytes(operation, request, source, phrases, text_item, text):
    return json.dumps({
        "schema": "recorder.transcription.v1", "status": "completed",
        "operation_id": operation, "pipeline": PIPELINE,
        "source": {"drive_id": request.drive_id, "item_id": request.item_id,
                   "sha1": request.source_sha1, "size": request.source_size,
                   "etag": source["eTag"], "name": source["name"],
                   "recorded_at": request.recorded_at,
                   "recorded_at_provenance": "device_hint",
                   "last_modified_at": source.get("lastModifiedDateTime")},
        "text_item_id": text_item["id"], "text_sha256": hashlib.sha256(text).hexdigest(),
        "text": " ".join(p["text"] for p in phrases), "phrases": phrases,
        "no_speech": not phrases,
    }, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


async def reconcile(graph, request, source, operation):
    parent, stem = source["parentReference"]["id"], source["name"][:-4]
    names = (stem + ".transcription.json", stem + ".txt")
    if any(len(name) > 255 for name in names):
        raise conflict()
    json_item = await graph.named_child(request.drive_id, parent, names[0])
    text_item = await graph.named_child(request.drive_id, parent, names[1])
    text = None
    if text_item:
        text = await graph.download(request.drive_id, text_item["id"], MAX_SIDECAR)
        recover_text(text, operation, request)
    if json_item:
        try:
            raw = await graph.download(request.drive_id, json_item["id"], MAX_SIDECAR)
            document = json.loads(raw)
        except (ValueError, RecursionError):
            raise conflict() from None
        if (not isinstance(document, dict) or document.get("schema") != "recorder.transcription.v1"
                or document.get("status") != "completed" or document.get("operation_id") != operation
                or document.get("pipeline") != PIPELINE):
            raise conflict()
        saved = document.get("source", {})
        if (not isinstance(saved, dict) or saved.get("drive_id") != request.drive_id
                or saved.get("item_id") != request.item_id or saved.get("sha1") != request.source_sha1
                or saved.get("size") != request.source_size or saved.get("etag") != source["eTag"]):
            raise IntelligenceError("source_changed", 409)
        if not text_item:
            # JSON is a completion marker, not permission to replace a user's deleted TXT.
            raise conflict()
        phrases = recover_text(text, operation, request)
        if (document.get("text_item_id") != text_item["id"]
                or document.get("text_sha256") != hashlib.sha256(text).hexdigest()
                or document.get("phrases") != phrases
                or document.get("text") != " ".join(p["text"] for p in phrases)
                or document.get("no_speech") is not (not phrases)):
            raise conflict()
    return names, json_item, text_item, text
