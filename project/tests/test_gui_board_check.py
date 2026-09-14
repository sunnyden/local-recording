import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest

path = Path(__file__).resolve().parents[1] / "firmware" / "tools" / "gui_board_check.py"
spec = importlib.util.spec_from_file_location("gui_board_check", path)
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


@pytest.mark.parametrize("action,value", [
    ("key", "format"), ("key", "enter\nui setup"), ("demo", "real-password"),
    ("memory", None), ("status", "extra"),
])
def test_only_whitelisted_commands(action, value):
    with pytest.raises(bench.BenchError):
        bench.wire_command(action, value)


def test_output_discards_unrecognized_and_sensitive_fields():
    value = {"v": 1, "event": "status", "screen": "home", "demo": True,
             "sensitive_setup": False, "password": "PRIVATE", "filename": "PRIVATE.opus"}
    result = bench.sanitized_response(value)
    assert "password" not in result and "filename" not in result


def test_duplicate_camera_safety_fields_rejected():
    with pytest.raises(ValueError):
        bench.unique_fields([("sensitive_setup", True), ("sensitive_setup", False)])


@pytest.mark.parametrize("status", [
    {"event": "status", "screen": "setup", "demo": False, "sensitive_setup": True},
    {"event": "status", "screen": "setup", "demo": False, "sensitive_setup": False},
    {"event": "status", "screen": "home", "demo": False},
    {"event": "status", "screen": "home", "demo": True, "sensitive_setup": "false"},
])
def test_refuses_sensitive_or_unknown_camera_state(status):
    with pytest.raises(bench.BenchError):
        bench.safe_camera_state(status)


def test_camera_only_captures_safe_synthetic_setup(tmp_path):
    status = {"event": "status", "screen": "setup", "demo": True, "sensitive_setup": False}
    board = SimpleNamespace(request=lambda command: status)
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        Path(command[-1]).write_bytes(b"synthetic image")
        return SimpleNamespace(returncode=0)

    output = tmp_path / "setup.png"
    bench.capture(board, output, ffmpeg="ffmpeg", run=run)
    assert output.read_bytes() == b"synthetic image"
    assert "-an" in calls[0]
    assert "video=c922 Pro Stream Webcam" in calls[0]
    assert not any(arg.startswith("audio=") for arg in calls[0])
    assert not list(tmp_path.glob("*.pending.png"))


@pytest.mark.parametrize("camera", [
    "Camera:audio=Microphone", "Camera=Other", "Camera\nOther", "", "x" * 129,
])
def test_camera_name_cannot_select_an_audio_device(tmp_path, camera):
    status = {"event": "status", "screen": "home", "demo": True, "sensitive_setup": False}
    board = SimpleNamespace(request=lambda command: status)
    with pytest.raises(bench.BenchError):
        bench.capture(board, tmp_path / "unsafe.png", camera=camera, ffmpeg="ffmpeg")


def test_discards_snapshot_if_display_becomes_sensitive(tmp_path):
    states = iter([
        {"event": "status", "screen": "home", "demo": True, "sensitive_setup": False},
        {"event": "status", "screen": "setup", "demo": False, "sensitive_setup": True},
    ])
    board = SimpleNamespace(request=lambda _: next(states))

    def run(command, **kwargs):
        Path(command[-1]).write_bytes(b"must not survive")
        return SimpleNamespace(returncode=0)

    with pytest.raises(bench.BenchError):
        bench.capture(board, tmp_path / "discard.png", ffmpeg="ffmpeg", run=run)
    assert not list(tmp_path.iterdir())


def test_serial_protocol_does_not_reset_or_echo_raw_logs():
    class Serial:
        def open(self):
            assert self.dtr is False and self.rts is False

        def reset_input_buffer(self):
            self.response = [b"PRIVATE UART LINE\nUI_TEST {\"v\":1,\"event\":\"pong\"}\n"]

        def write(self, command):
            assert command == b"ui ping\n"
            return len(command)

        def read(self, size):
            return self.response.pop(0)

        def close(self):
            pass

    board = bench.Board("COM3", factory=Serial)
    assert board.request("ping") == {"v": 1, "event": "pong"}
    board.close()
