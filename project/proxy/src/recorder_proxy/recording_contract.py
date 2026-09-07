from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
import re

from .graph import identifier
from .intelligence_errors import IntelligenceError
from .speech import SPEECH_VERSION

MAX_WAV_BYTES = 1800 * 16000 * 2 + 65536
PIPELINE = {"name": "recorder-fast-transcription", "version": 1,
            "speech_api_version": SPEECH_VERSION, "language": "auto-multilingual",
            "format": "pcm16-mono-16000", "max_duration_seconds": 1800}


@dataclass(frozen=True)
class RecordingRequest:
    drive_id: str
    item_id: str
    source_sha1: str
    source_size: int
    recorded_at: str | None = None

    @classmethod
    def parse(cls, value):
        required = {"v", "drive_id", "item_id", "source_sha1", "source_size"}
        if (not isinstance(value, dict) or not required <= value.keys()
                or value.keys() - required - {"recorded_at"}
                or type(value["v"]) is not int or value["v"] != 1
                or type(value["source_size"]) is not int or value["source_size"] < 1
                or not isinstance(value["source_sha1"], str)
                or not re.fullmatch("[0-9a-f]{40}", value["source_sha1"])):
            raise IntelligenceError("invalid_request", 400)
        if value["source_size"] > MAX_WAV_BYTES:
            raise IntelligenceError("recording_too_long", 413)
        recorded_at = value.get("recorded_at")
        if recorded_at is not None:
            try:
                if (not isinstance(recorded_at, str) or len(recorded_at) > 40
                        or not re.fullmatch(
                            r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:Z|[+-]\d{2}:\d{2})",
                            recorded_at)
                        or datetime.fromisoformat(recorded_at).tzinfo is None):
                    raise ValueError()
            except ValueError:
                raise IntelligenceError("invalid_request", 400) from None
        return cls(identifier(value["drive_id"]), identifier(value["item_id"]),
                   value["source_sha1"], value["source_size"], recorded_at)

    def operation(self, owner):
        data = [owner, self.drive_id, self.item_id, self.source_sha1, PIPELINE]
        return hashlib.sha256(json.dumps(data, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def verify_source(item, request):
    if "file" not in item or not item["name"].lower().endswith(".wav"):
        raise IntelligenceError("unsupported_audio", 415)
    if type(item.get("size")) is not int or item["size"] > MAX_WAV_BYTES:
        raise IntelligenceError("recording_too_long", 413)
    if item["size"] != request.source_size:
        raise IntelligenceError("source_changed", 409)
    hashes = item.get("file", {}).get("hashes", {})
    if not isinstance(hashes, dict):
        raise IntelligenceError("source_changed", 409)
    sha1 = hashes.get("sha1Hash")
    if sha1 is not None and (not isinstance(sha1, str) or sha1.lower() != request.source_sha1):
        raise IntelligenceError("source_changed", 409)
    if not isinstance(item.get("eTag"), str) or not 1 <= len(item["eTag"]) <= 512:
        raise IntelligenceError("source_changed", 409)
    parent = item.get("parentReference", {}).get("id")
    identifier(parent)
    return parent
