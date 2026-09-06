class CompanionError(Exception):
    """A deliberately redacted error safe to show in the terminal."""


class TransportError(CompanionError):
    """The connection/session is unusable; never replay encrypted traffic."""


class ProtocolError(CompanionError):
    """The device response violates the pinned protocol."""

DEVICE_ERROR_MESSAGES = {
    "auth_unavailable": "Authorization is not ready. Check Wi-Fi with status; configure wifi and allow time synchronization before retrying graph.",
    "invalid_resource": "The requested authorization resource is invalid.",
    "invalid_request": "The device rejected an invalid request.",
    "unknown_type": "The device does not support this command.",
    "confirmation_required": "The operation requires explicit confirmation.",
    "storage_error": "The device could not update credential storage.",
}


class DeviceError(CompanionError):
    """A valid authenticated rejection; the secure transport remains usable."""

    def __init__(self, code):
        self.code = code if code in DEVICE_ERROR_MESSAGES else "unknown"
        detail = DEVICE_ERROR_MESSAGES.get(self.code, "The device reported an unrecognized error.")
        super().__init__("Recorder rejected the request: " + detail)


CLEANUP_FAILURE_NOTE = "BLE cleanup also failed; check Bluetooth before reconnecting."


async def cleanup_preserving_failure(cleanup, primary):
    """Retain secondary cleanup failures without replacing the active exception."""
    try:
        await cleanup()
    except Exception as cleanup_error:
        # This cleanup-only boundary retains even an unexpected backend failure.
        # Cancellation is a BaseException and must continue to propagate.
        primary.add_note(CLEANUP_FAILURE_NOTE)
        primary.cleanup_failed = True
        primary.cleanup_error = cleanup_error
