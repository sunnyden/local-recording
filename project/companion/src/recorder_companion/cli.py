import argparse
import asyncio
import getpass
import logging
import sys
import uuid
import warnings
import webbrowser

from .ble import DEFAULT_SERVICE_UUID, BleTransport, discover
from .client import RecorderClient
from .errors import CLEANUP_FAILURE_NOTE, CompanionError, cleanup_preserving_failure
from .protocol import AUTH_STATES, verification_uri


def hidden(prompt):
    # getpass otherwise falls back to echoing on redirected/unavailable terminals.
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        try:
            return getpass.getpass(prompt)
        except getpass.GetPassWarning:
            raise CompanionError("A real terminal is required for hidden credential input.") from None


def safe_label(value):
    return "".join(c if c.isascii() and c.isprintable() else "?" for c in value)[:80]


async def show_code(uri, code):
    uri = verification_uri(uri)
    print(f"Sign in at {uri}\nEnter this user code: {code}")
    print("Use only your browser. The companion never asks for your Microsoft password.")
    if input("Open the Microsoft page in your default browser? [y/N] ").strip().lower() == "y":
        if not webbrowser.open(uri, new=2):
            print("Browser did not open; use the displayed address manually.")


async def run(args):
    devices = await discover(args.service_uuid)
    if args.command == "scan":
        for item in devices:
            print(f"{safe_label(item.address)}  {safe_label(item.name)}")
        if not devices:
            print("No recorder found. Verify firmware setup gates, then enter Setup and scan again.")
        return
    if args.address:
        devices = [item for item in devices if item.address.lower() == args.address.lower()]
    if not devices:
        raise CompanionError(
            "No matching recorder advertises the provisioning service; check firmware setup gates."
        )
    if len(devices) != 1:
        raise CompanionError("Multiple recorders found. Scan, then select --address explicitly.")
    print(f"Connecting to {safe_label(devices[0].name)} ({safe_label(devices[0].address)}).")
    client = RecorderClient(BleTransport(devices[0].device, args.service_uuid))

    async def connect():
        username = hidden("Setup username displayed on recorder: ")
        password = hidden("Setup password displayed on recorder: ")
        try:
            await client.connect(username, password)
        finally:
            # Python strings cannot be guaranteed securely erased. No persistence.
            username = password = None

    try:
        await connect()
        print("Authenticated. Keep Setup open on the recorder until Finish.")
        while True:
            action = input(
                "\nCommand [wifi/status/graph/proxy/cancel-graph/cancel-proxy/"
                "unlink/reconnect/finish/quit]: "
            ).strip().lower()
            try:
                if action in {
                    "wifi", "status", "graph", "proxy", "cancel-graph",
                    "cancel-proxy", "unlink", "finish",
                } and client.security is None:
                    raise CompanionError("Use reconnect first; no secure session is active.")
                if action == "wifi":
                    ssid = input("Wi-Fi SSID: ")
                    password = hidden("Wi-Fi password (empty for open network): ")
                    try:
                        print("Wi-Fi:", await client.configure_wifi(ssid, password))
                    finally:
                        password = None
                elif action == "status":
                    print("Wi-Fi:", await client.wifi_status())
                    status = await client.request("status")
                    if status.get("wifi") in {"connected", "disconnected"}:
                        print("Device IP connection:", status["wifi"])
                    if type(status.get("time_valid")) is bool:
                        print("Device time:", "ready" if status["time_valid"] else "waiting for synchronization")
                    for resource in ("graph", "proxy"):
                        result = await client.request("auth.status", resource=resource)
                        state = result["state"]
                        print(f"{resource}: {state if state in AUTH_STATES else 'unknown'}")
                elif action in {"graph", "proxy"}:
                    print(f"{action}: {await client.authorize(action, show_code)}")
                elif action in {"cancel-graph", "cancel-proxy"}:
                    await client.request("auth.cancel", resource=action.removeprefix("cancel-"))
                    print("Authorization cancelled.")
                elif action == "unlink":
                    if input("Erase the recorder's saved account? Type UNLINK to confirm: ") == "UNLINK":
                        await client.request("auth.unlink", confirm=True)
                        print("Account unlinked.")
                    else:
                        print("Unlink cancelled.")
                elif action == "reconnect":
                    await connect()
                    print("Fresh secure session established. Check status; actions were not replayed.")
                elif action == "finish":
                    await client.request("setup.finish")
                    print("Setup finished.")
                    return
                elif action == "quit":
                    print("Disconnected without Finish. The recorder's setup timer will expire.")
                    return
                else:
                    print("Choose one of the displayed commands.")
            except CompanionError as error:
                report_error(error)
                if client.security is None:
                    print("Use reconnect before another command; uncertain commands are never replayed.")
                else:
                    print("The secure connection is still usable. Use status to check the device.")
    finally:
        primary = sys.exception()
        if primary is None:
            await client.close()
        else:
            await cleanup_preserving_failure(client.close, primary)


def report_error(error):
    print(str(error), file=sys.stderr)
    for note in getattr(error, "__notes__", ()):
        print(note, file=sys.stderr)


def report_cleanup_failure(error):
    # asyncio.run may translate a cancelled main task into KeyboardInterrupt.
    if (getattr(error, "cleanup_failed", False)
            or getattr(error.__cause__, "cleanup_failed", False)):
        print(CLEANUP_FAILURE_NOTE, file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Secure recorder BLE setup (Windows)")
    parser.add_argument("command", choices=("scan", "setup"))
    parser.add_argument("--address", help="Exact address selected from scan (not a credential)")
    parser.add_argument("--service-uuid", default=DEFAULT_SERVICE_UUID,
                        help="Firmware provisioning service UUID")
    args = parser.parse_args(argv)
    try:
        args.service_uuid = str(uuid.UUID(args.service_uuid))
    except ValueError:
        parser.error("--service-uuid must be a UUID")
    # Never enable packet/debug logs: protobuf and BLE buffers contain credentials.
    logging.disable(logging.CRITICAL)
    try:
        asyncio.run(run(args))
    except CompanionError as error:
        report_error(error)
        return 1
    except (KeyboardInterrupt, EOFError) as error:
        print("Setup interrupted; no pending command is replayed.", file=sys.stderr)
        report_cleanup_failure(error)
        return 130
    except Exception as error:
        print("Setup failed. Check the recorder display and reconnect.", file=sys.stderr)
        report_cleanup_failure(error)
        return 1
    return 0
