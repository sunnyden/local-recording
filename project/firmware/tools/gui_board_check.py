"""Opt-in bench UI commands and guarded, video-only board-camera snapshots."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
from uuid import uuid4

KEYS = ("up", "down", "enter", "back")
SCENES = ("home", "recording", "recordings", "playback", "sync", "ai-listening",
          "ai-speaking", "setup", "error", "calibration", "off")
STATUS_FIELDS = {
    "v", "event", "screen", "selected", "mode", "demo", "sensitive_setup",
    "heap_free", "heap_min", "dma_largest", "overruns", "voice_state", "sync_phase",
    "gui_ram_bytes", "asset_bytes", "font_bytes", "display_dma_bytes",
}


class BenchError(Exception):
    pass


def unique_fields(items):
    value = {}
    for key, item in items:
        if key in value:
            raise ValueError("duplicate field")
        value[key] = item
    return value


def wire_command(action, value=None):
    if action in ("ping", "status") and value is None:
        return f"ui {action}\n".encode("ascii")
    if action == "key" and value in KEYS:
        return f"ui key {value}\n".encode("ascii")
    if action == "demo" and value in SCENES:
        return f"ui demo {value}\n".encode("ascii")
    raise BenchError("Unsupported bench UI command")


def sanitized_response(value):
    if not isinstance(value, dict) or type(value.get("v")) is not int or value["v"] != 1:
        raise BenchError("Invalid UI test response")
    event = value.get("event")
    if event == "error":
        raise BenchError("Recorder rejected the test command")
    if event not in ("status", "key", "demo", "pong"):
        raise BenchError("Unexpected UI test event")
    result = {}
    for key in STATUS_FIELDS:
        if key not in value:
            continue
        field = value[key]
        if not isinstance(field, (str, int, bool)) or isinstance(field, str) and (
                len(field) > 64 or any(ord(c) < 32 for c in field)):
            raise BenchError("Invalid UI status field")
        result[key] = field
    return result


class Board:
    def __init__(self, port, timeout=6, *, factory=None, clock=time.monotonic):
        if not re.fullmatch(r"COM[1-9][0-9]*", port, re.IGNORECASE):
            raise BenchError("Specify the intended Windows COM port explicitly")
        if factory is None:
            try:
                import serial
            except ImportError:
                raise BenchError("Run with the ESP-IDF Python environment containing pyserial") from None
            factory = serial.Serial
        self.serial = factory()
        self.serial.port, self.serial.baudrate = port, 115200
        self.serial.timeout, self.serial.write_timeout = 0.1, 2
        self.serial.dtr = self.serial.rts = False
        self.serial.open()
        self.timeout, self.clock = timeout, clock

    def close(self):
        self.serial.close()

    def request(self, action, value=None):
        command = wire_command(action, value)
        self.serial.reset_input_buffer()
        if self.serial.write(command) != len(command):
            raise BenchError("Incomplete serial command write")
        deadline, pending, dropping = self.clock() + self.timeout, bytearray(), False
        expected = "pong" if action == "ping" else action
        while self.clock() < deadline:
            for byte in self.serial.read(256):
                if byte == 10:
                    line = bytes(pending).rstrip(b"\r")
                    pending.clear()
                    if dropping:
                        dropping = False
                        continue
                    if not line.startswith(b"UI_TEST "):
                        continue
                    try:
                        response = sanitized_response(json.loads(
                            line[8:].decode("utf-8"), object_pairs_hook=unique_fields))
                    except (ValueError, RecursionError):
                        raise BenchError("Invalid UI test JSON; raw data is not printed") from None
                    if response["event"] == expected:
                        return response
                elif not dropping:
                    if len(pending) >= 4096:
                        pending.clear()
                        dropping = True
                    else:
                        pending.append(byte)
        raise BenchError("No matching UI response; test-input firmware may not be installed")


def safe_camera_state(status):
    if (status.get("event") != "status" or type(status.get("sensitive_setup")) is not bool
            or type(status.get("demo")) is not bool or not isinstance(status.get("screen"), str)):
        raise BenchError("Recorder did not report a camera-safe display state")
    if status["sensitive_setup"] or not status["demo"]:
        raise BenchError("Snapshots are restricted to synthetic demo states")
    return status["screen"], status["demo"]


def capture(board, output, *, camera="c922 Pro Stream Webcam", ffmpeg=None, crop=None,
            run=subprocess.run):
    before = safe_camera_state(board.request("status"))
    output = Path(output).resolve()
    if output.exists():
        raise BenchError("Snapshot destination already exists")
    if output.suffix.lower() != ".png":
        raise BenchError("Snapshot output must be a PNG")
    if not isinstance(camera, str) or not re.fullmatch(r"[A-Za-z0-9 ._()\-]{1,128}", camera):
        raise BenchError("Invalid camera name")
    ffmpeg = ffmpeg or shutil.which("ffmpeg")
    if not ffmpeg:
        raise BenchError("ffmpeg is required for a video-only DirectShow snapshot")
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_name(f".{output.stem}-{uuid4().hex}.pending.png")
    command = [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-f", "dshow",
               "-video_size", "1280x720", "-framerate", "30", "-vcodec", "mjpeg",
               "-i", f"video={camera}", "-ss", "2", "-frames:v", "1", "-an", "-update", "1"]
    if crop:
        if not re.fullmatch(r"[0-9]+:[0-9]+:[0-9]+:[0-9]+", crop):
            raise BenchError("Crop must be width:height:x:y")
        w, h, x, y = map(int, crop.split(":"))
        if min(w, h) <= 0 or x + w > 1280 or y + h > 720:
            raise BenchError("Crop is outside the selected video size")
        command += ["-vf", f"crop={crop}"]
    command += ["-n", str(pending)]
    try:
        try:
            result = run(command, capture_output=True, timeout=20, check=False)
        except (OSError, subprocess.TimeoutExpired):
            raise BenchError("Camera capture could not complete") from None
        if result.returncode or not pending.is_file():
            raise BenchError("Camera capture failed; check the selected video device")
        after = safe_camera_state(board.request("status"))
        if before != after:
            raise BenchError("Display changed during capture; snapshot discarded")
        pending.rename(output)
        return {"event": "capture", "screen": after[0], "demo": after[1], "path": str(output)}
    finally:
        pending.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("ping")
    actions.add_parser("status")
    actions.add_parser("key").add_argument("value", choices=KEYS)
    actions.add_parser("demo").add_argument("value", choices=SCENES)
    photo = actions.add_parser("capture")
    photo.add_argument("--output", required=True)
    photo.add_argument("--camera", default="c922 Pro Stream Webcam")
    photo.add_argument("--ffmpeg")
    photo.add_argument("--crop")
    args = parser.parse_args()
    board = Board(args.port)
    try:
        if args.action == "capture":
            result = capture(board, args.output, camera=args.camera,
                             ffmpeg=args.ffmpeg, crop=args.crop)
        else:
            result = board.request(args.action, getattr(args, "value", None))
        print(json.dumps(result))
    finally:
        board.close()


if __name__ == "__main__":
    try:
        main()
    except (BenchError, OSError) as exc:
        message = str(exc) if isinstance(exc, BenchError) else "Bench device I/O failed"
        print(json.dumps({"event": "error", "message": message}))
        raise SystemExit(1) from None
