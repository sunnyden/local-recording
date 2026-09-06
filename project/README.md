# Embedded recorder

ESP32-S3 WAV recorder, OneDrive uploader and hands-free Azure Voice Live client
for the ATK_DNESP32S3_V1.4 board.

The implementation is split into:

| Directory | Responsibility |
| --- | --- |
| `firmware\` | ESP-IDF 6.1 board drivers, recording/playback, menu, BLE, identity and cloud clients |
| `companion\` | Windows Python BLE provisioning and device-code sign-in helper |
| `proxy\` | Authenticated, bounded Python WebSocket bridge to Azure Voice Live |
| `infra\` | Secretless app registration, Foundry and Container Apps deployment |
| `protocols\` | Shared BLE/voice contracts and binary fixtures |
| `docs\` | Architecture, operations and deployment notes |

Recordings are signed PCM16 WAV, 16000 Hz, mono. The user selects recording,
playback, synchronization or AI as mutually exclusive modes. AI capture and
speaker playback are simultaneous; Voice Live owns VAD and echo cancellation.
KEY3 moves up, KEY1 down, KEY0 enters/confirms, KEY2 exits/goes back.

OneDrive synchronization is explicitly selected and retains the local WAV.
The destination is root-level `local-recording/`, not the special AppFolder.
MP3, background sync, wake-word activation and actual remote tool execution
are outside this MVP.

## Identity and privacy

Public client A obtains separate user-delegated tokens for Microsoft Graph
and proxy API B. The ESP owns device-code polling and token refresh. The BLE
companion displays the verification URI/user code, but does not receive the
polling device code or OAuth tokens. Sign-in/consent remains a browser action.

The proxy accepts only API B tokens from the configured client/user and calls
Voice Live using managed identity. It does not receive Graph tokens or stored
recordings. No Azure API key or OAuth client secret is needed.

Wi-Fi passwords and refresh tokens are still credentials. Do not store real
credentials on an unprotected development board without explicitly accepting
the physical-access risk. Enabling flash protection/eFuse features is a
separate, potentially irreversible operation and is never automatic.

## Setup order

1. Follow the firmware documentation/build helper to select the installed
   SDK Python and tool environment for `C:\esp\v6.1\esp-idf`. Calling
   `export.ps1` directly under the system Python can select a nonexistent
   virtual environment. Keep activation and `idf.py` in the same process.
   Discover the CH340 port; do not assume COM3 forever.
2. Follow `infra\README.md` to register client A/API B and provision approved
   resources. Preserve the ignored deployment state files.
3. Build the proxy and configure its explicit consumer-user allowlist. Read
   `proxy\README.md`; do not expose an unauthenticated endpoint for convenience.
4. Use the companion to establish protected BLE provisioning and complete
   Microsoft sign-in. Hardware security and user consent cannot be replaced
   by mocks or hidden automatic enrollment.
5. Exercise local recording/playback, OneDrive sync, and hands-free AI using
   the component documentation and acceptance criteria.

Component READMEs describe the actual build/run commands and any outstanding
implementation constraints. See `docs\cloud-setup.md` for cloud resources
created during implementation and the distinction between configuration
acceptance and full acoustic/end-to-end validation.

`tools\inspect-board.ps1` discovers the CH340 without opening it. Supplying
`-AllowReset` additionally reads ROM chip/flash information and resets the
board; it never flashes, erases, or changes eFuses. The working SDK activation
profile on this machine is
`C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`.

## Shared wire protocols

- [Device-to-proxy voice](protocols/voice-v1.md)
- [BLE provisioning](protocols/ble-v1.md)
- [Canonical binary fixtures](protocols/voice-v1-fixtures.json)

Versioned text controls and bounded binary PCM frames keep provider-specific
payloads and base64 overhead off the ESP. All provider configuration is
controlled by the proxy.
