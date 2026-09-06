import warnings
import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from recorder_companion import cli
from recorder_companion.errors import CompanionError


def test_cli_has_no_secret_arguments(capsys):
    with pytest.raises(SystemExit) as result:
        cli.main(["--help"])
    assert result.value.code == 0
    help_text = capsys.readouterr().out
    assert "--password" not in help_text
    assert "--token" not in help_text


def test_hidden_input_refuses_echo_fallback(monkeypatch):
    def fallback(prompt):
        warnings.warn("echo", cli.getpass.GetPassWarning)
        return "secret"
    monkeypatch.setattr(cli.getpass, "getpass", fallback)
    with pytest.raises(CompanionError, match="real terminal"):
        cli.hidden("Password: ")


def test_cli_redacts_unexpected_exceptions(monkeypatch, capsys):
    monkeypatch.setattr(cli, "run", AsyncMock(side_effect=RuntimeError("my-secret-token")))
    assert cli.main(["scan"]) == 1
    assert "my-secret-token" not in capsys.readouterr().err


def test_cli_reports_secondary_cleanup_failure(monkeypatch, capsys):
    error = CompanionError("Primary operation failed.")
    error.add_note("BLE cleanup also failed; check Bluetooth before reconnecting.")
    monkeypatch.setattr(cli, "run", AsyncMock(side_effect=error))
    assert cli.main(["scan"]) == 1
    output = capsys.readouterr().err
    assert "Primary operation failed." in output
    assert "BLE cleanup also failed" in output


def test_cli_reports_cleanup_failure_when_asyncio_translates_cancellation(monkeypatch, capsys):
    cancelled = asyncio.CancelledError()
    cancelled.cleanup_failed = True
    interrupted = KeyboardInterrupt()
    interrupted.__cause__ = cancelled
    def fail_run(*args):
        raise interrupted
    monkeypatch.setattr(cli, "run", fail_run)
    assert cli.main(["scan"]) == 130
    assert "BLE cleanup also failed" in capsys.readouterr().err


def test_advertisement_cannot_inject_terminal_control_codes():
    assert "\x1b" not in cli.safe_label("recorder\x1b[31m")


def test_empty_scan_explains_default_firmware_gate(monkeypatch, capsys):
    monkeypatch.setattr(cli, "discover", AsyncMock(return_value=[]))
    asyncio.run(cli.run(SimpleNamespace(command="scan", service_uuid="uuid")))
    assert "firmware setup gates" in capsys.readouterr().out


def test_browser_requires_approved_url_and_human_confirmation(monkeypatch):
    opened = []
    monkeypatch.setattr(cli.webbrowser, "open", lambda uri, **kw: opened.append(uri) or True)
    monkeypatch.setattr("builtins.input", lambda _: "n")
    asyncio.run(cli.show_code("https://microsoft.com/devicelogin", "ABCD-EFGH"))
    assert not opened
    monkeypatch.setattr("builtins.input", lambda _: "y")
    asyncio.run(cli.show_code("https://microsoft.com/devicelogin", "ABCD-EFGH"))
    assert opened == ["https://microsoft.com/devicelogin"]
    with pytest.raises(CompanionError):
        asyncio.run(cli.show_code("https://attacker.example/devicelogin", "ABCD-EFGH"))
    assert len(opened) == 1


@pytest.mark.parametrize("confirmation,unlinked", [("yes", False), ("UNLINK", True)])
def test_unlink_requires_exact_confirmation(monkeypatch, confirmation, unlinked):
    calls = []
    class FakeClient:
        def __init__(self, transport):
            self.security = object()

        async def connect(self, username, password):
            pass

        async def close(self):
            pass

        async def request(self, kind, **fields):
            calls.append((kind, fields))

    inputs = iter(["unlink", confirmation, "finish"])
    monkeypatch.setattr("builtins.input", lambda _: next(inputs))
    monkeypatch.setattr(cli, "hidden", lambda _: "secret")
    monkeypatch.setattr(cli, "RecorderClient", FakeClient)
    device = SimpleNamespace(device=object(), name="Recorder", address="address")
    monkeypatch.setattr(cli, "discover", AsyncMock(return_value=[device]))
    asyncio.run(cli.run(SimpleNamespace(command="setup", address=None, service_uuid="uuid")))
    assert (("auth.unlink", {"confirm": True}) in calls) is unlinked
    assert calls[-1] == ("setup.finish", {})
