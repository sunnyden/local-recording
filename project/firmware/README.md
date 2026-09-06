# ESP32-S3 recorder firmware

ESP-IDF **6.1**, ATK_DNESP32S3_V1.4. Source and dependencies are built; the
application has not been flashed or functionally validated on hardware. The
firmware implementation agent performed build/host work without opening a
serial port. Separately, the parent completed the ROM identification probe below.
No flash writes, erase, eFuse writes, or real credential enrollment occurred.

### Parent ROM identification — 2026-09-07

Using the working Espressif profile and esptool 5.3.1 `--no-stub` chip/flash
identification commands on **COM3**, the parent reported:

| Item | ROM identification |
| --- | --- |
| Chip | ESP32-S3 QFN56 revision 0.2 |
| Embedded PSRAM | 8 MB, AP 3.3 V |
| Flash | 16 MB, quad, 3.3 V |
| Reset effects | Two normal RTS resets |
| Mutations/access | No flash writes/erase, eFuse writes, or credential reads |

ROM identification confirms reported capacities, not successful PSRAM runtime
initialization at the configured octal/80 MHz setting. Physical LCD/P5 jumper
approval, microphone/speaker operation, and acoustics remain unverified.
The parent will not flash an application without physical-jumper approval.

## Build and tests

```powershell
.\tools\build.ps1
.\tools\test-core.ps1
```

The build script activates the existing installation at
`C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`; override
`-ActivationScript` for another installation. It invokes only the activated
environment's **absolute Python executable** with `tools\idf.py build`; it does
not resolve `python` or `idf.py` through an unrelated system installation.
The dependency manifest and `dependencies.lock` pin network provisioning 1.2.4,
WebSocket client 1.8.0, and cJSON 1.7.19~2.

Working installed environment on this machine:

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
$py = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
& $py --version
& $py -m esptool version
```

These two checks do not open a serial port. The verified versions are Python
3.14.5 and esptool 5.3.1. The important distinction is the **existing SDK venv
path**, not avoiding Python 3.14 categorically. Calling the generic SDK
`export.ps1` without the installation profile incorrectly searches under
`C:\Users\exede\.espressif\python_env\idf6.1_py3.14_env`, which does not exist.
Do not create a second environment to work around that path-selection error.
The working profile sets `IDF_PATH=C:\esp\v6.1\esp-idf`,
`IDF_TOOLS_PATH=C:\Espressif\tools`, and
`IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.1\venv`.

The portable C tests use Zig 0.14.1 as a host compiler. The initial MSVC test
attempt failed because the machine's configured VS path no longer exists.
An isolated compiler was then installed beneath ignored `.host-tools`:

```powershell
python -m pip install --target .host-tools ziglang==0.14.1
.\tools\test-core.ps1
```

This is a test-only tool, not a firmware runtime dependency. Eleven host executables
cover:

* WAV encoding/parsing, odd-sample recovery, format rejection, checkpoint
  corruption, voice headers/sequence/sample alignment, and 320 KiB range math.
* The production identity and BLE control sources with synthetic token/storage/
  scheduler mocks: pending/slow-down/denial/expiry/cancellation, separate audiences,
  refresh rotation/revocation, persistence failure, and malformed endpoint requests.
* The production upload loop with a deterministic HTTP transport mock: small
  partial socket writes, 320 KiB ranges, uncertain commits, server-confirmed
  offsets, Retry-After, expiration/quota, cancellation, and malformed ranges.
* The production HTTPS helper with bounded request-line/bearer headers and
  network/time-readiness guards.
* The production Wi-Fi startup path with persisted configuration, connection
  retry limits, and stop-state handling.
* The production voice client with a fake WebSocket/audio boundary: API-B token
  headers, hello/ready order, transport fragmentation, continuing microphone
  capture during playback, completed-sample clear acknowledgments, stale epochs,
  malformed PCM frames, and clean session teardown.
* Authoritative `project\protocols\voice-v1-fixtures.json`: every valid and invalid
  wire fixture is consumed directly, valid decoded fields/signed samples are
  checked, and encoder output must match the canonical bytes exactly.
* The production I2S implementation with simulated DMA callbacks: six software
  frames plus four DMA periods enforce the 3200-sample budget, moving samples
  into DMA does not return credit, only completed samples free capacity, clear
  zeros pending DMA, short tails count exactly, and stereo/mono slot conversion
  is preserved.
* Three credential-store profile executables: default-denied, explicit plaintext
  development, and existing-HMAC encrypted NVS. Fourteen scenarios cover missing/
  unsuitable/unprotected keys, derivation/init failures, success, bounded reads,
  and absence of key-generation or generic auto-generating initialization calls.

Mocks never contact a service or persist credentials. They do not validate the
real TLS, Bluetooth, SD, DMA, radio, or acoustic behavior.

## Safety gates

The default build intentionally does **not** drive GPIO40 or initialize Wi-Fi
credential persistence.

* Verify the physical jumper routing in [board notes](docs/board.md), then
  explicitly set `CONFIG_RECORDER_SPI_LCD_CONFIRMED=y` using menuconfig.
* Before storing real Wi-Fi credentials or Microsoft refresh tokens, the owner
  must choose a protection policy. The currently implemented opt-in
  `CONFIG_RECORDER_DEVELOPMENT_CREDENTIAL_RISK=y` permits ordinary NVS and
  explicitly accepts physical extraction risk. **It is not encrypted NVS.**
  Alternatively, the optional [existing-HMAC profile](docs/credential-profiles.md)
  uses an **already-provisioned**, protected HMAC_UP key to derive encrypted NVS
  keys. It fails closed if prerequisites are absent and never provisions a key.
  The default still fails closed with `ESP_ERR_NOT_ALLOWED`; no automatic
  downgrade, root-key generation, flash-encryption enablement, or eFuse writes run.
* `CONFIG_RECORDER_CLIENT_ID` is the public client A UUID.
* `CONFIG_RECORDER_PROXY_SCOPE` is exactly
  `api://<API-B-client-id>/access_as_user` (the firmware adds `offline_access`).
* `CONFIG_RECORDER_PROXY_URL` is the authenticated `wss://.../v1/voice` endpoint.
  Query strings, userinfo, and fragments are rejected.

These three public values are configuration, not secrets. Never put passwords,
setup credentials, tokens, or signed upload URLs in sdkconfig or source control.
The separate `sdkconfig.lcd-validation.defaults` profile is **compile-only** and
does not authorize flashing a board with unverified jumpers.

Public registration defaults, verified against `project\infra\.state\identity.json`:

| Setting | Default |
| --- | --- |
| Public client A | `bdbb7442-7892-471e-9ee4-95d4e9c02a37` |
| API B delegated scope | `api://30f56876-0c90-48b8-aff0-654b5000fd43/access_as_user` |
| Graph delegated scope | `https://graph.microsoft.com/Files.ReadWrite` |
| Authority | `https://login.microsoftonline.com/consumers` |
| Proxy WSS URL | Unset until the authenticated proxy is deployed |

The firmware adds `offline_access` separately to each resource request. These
defaults do not enable Wi-Fi, enrollment, or token storage while the credential
policy gate is closed. Host tests continue to use distinct synthetic application
IDs and tokens, not the real registrations.

## Behavior

KEY3 up, KEY1 down, KEY0 select, KEY2 back. Modes are mutually exclusive.
Record and playback also accept KEY0 to stop. Setup, Sync, and AI stop on KEY2.
The LCD uses 26-character lines, a small original uppercase font, and one
10 KiB internal-DMA stripe; no framebuffer is allocated.

* **Record:** signed PCM16, 16 kHz mono WAV. Stereo I2S ADC slots are reduced to
  the left microphone slot. A 96-frame PSRAM queue is approximately 60 KiB.
  Overrun, short read/write, queue overflow, and storage errors stop recording.
* **Recordings:** bounded-memory directory traversal and WAV chunk parser.
  Unsupported formats are rejected, not played at a wrong rate.
* **Sync:** explicit, direct Microsoft Graph requests; root `local-recording`
  folder; conflict-fail upload sessions; 320 KiB ranges streamed through 4 KiB.
  Graph bearer tokens never accompany signed upload PUTs. Local WAVs remain.
* **AI:** API-B bearer WSS, `recorder.voice.v1`, exact 24-byte ERV1 header and
  16 kHz PCM. Simultaneous raw microphone capture and playback; no local AEC,
  VAD, resampling, or provider-specific protocol. The proxy owns those choices.
* **Setup:** physical entry, random displayed username and 96-bit random
  password, protocomm Security 2 only, 10-minute lifetime. Standard Wi-Fi
  endpoints plus encrypted `recorder-control`. No password printed to UART.
  Provisioning remains open after Wi-Fi success; explicit finish/back/timeout
  closes it without permanently releasing Bluetooth memory, permitting reopen.

The pinned transport is NimBLE protocomm Security **2, patch 1**. Discover the
version through `proto-ver`; service UUID is
`1775244d-6b43-439b-877c-060f2d9bed07`, explicitly pinned in firmware and
confirmed from the real board advertisement. Endpoint descriptors identify
`recorder-control`; the companion does not assume characteristic UUIDs.
IDF 6.1 limits each GATT characteristic value to **512 encrypted bytes**, leaving
at most **496 plaintext bytes** with the AES-GCM tag. The application endpoint
now enforces this same **496-byte request and response limit**, aligned with the
shared BLE contract. Host tests accept exactly 496 bytes and reject 497 bytes.
Use ATT reliable long writes/long reads within 512
bytes; never split independently encrypted ciphertext into ordinary writes.

Microsoft device grants and refresh execute on the ESP. The companion gets
only user-facing verification URI/code, expiry, and state. Each resource has
its own access/refresh cache. Replacement refresh tokens commit before use.
The authority is intentionally `consumers`; organizational support is deferred.
There are no client secrets, API keys, ID-token authorization, or PC token cache.

`auth.start` can return `{"ok":true,"state":"pending"}` before Microsoft's
asynchronous device-code request finishes. Poll `auth.status`: once available,
its pending response includes `user_code`, `verification_uri`, and remaining
`expires_in`, in addition to `v`, echoed `id`, `ok`, and `state`. Repeating
`auth.start` while pending returns the same public fields, not a second grant.
Authorized/denied/expired/error responses do not include the user code.

## Recovery and limits

Recording IDs use random values, not an untrusted wall clock. New recordings
are `.part`; finalized files are `.wav`. Every second, PCM is flushed/fsynced
before an alternating, checksummed 16-byte checkpoint is fsynced in `.ckp`.
At boot only canonical recorder `.part` files with valid checkpoints are
repaired, truncated to the last committed complete sample, and renamed.
This can lose approximately one second after power loss. Damaged/missing
checkpoints leave the `.part` file intact and increment the failure count.
The card is never formatted or recordings automatically deleted. FAT itself is
not power-fail atomic; an offline filesystem check may still be necessary.

Maximum data length is `0x7fff0000`, below signed 32-bit seek limits; stop rather
than cross RIFF/FAT/stdio limits. Catalog memory does not scale with file count.

Uploads keep signed session URLs only in RAM. Restart starts a new session
unless the remote final item can be reconciled. A protected NVS journal binds
the most recent confirmed completion to drive/name/SHA-1; older completions are
checked remotely. Existing or ambiguous final items are accepted only with
matching name, size, and Graph SHA-1. If the drive does not expose `sha1Hash`,
reconciliation stops visibly instead of duplicating or overwriting a file.
Throttling/transient range failures use bounded backoff and server range queries.
Numeric `Retry-After` up to 120 seconds is honored with cancellation checks;
longer or date-form retry values stop the attempt rather than retry early.
Session expiry, consent/quota errors, and unsupported reconciliation require a
new explicit Sync attempt. This is not an unlimited background retry service.

The frozen v1 voice contract uses **3200 samples / 200 ms outstanding**, a
six-frame software ring (3840 bytes), four 20 ms DMA periods, PCM-rate proxy
pacing, and startup/catch-up bursts no larger than 60 ms. The whole 200 ms credit
must not be burst into the initially empty software ring. A separate pending
counter includes **both software and DMA**, so moving a frame into DMA does not
return playback credit. Internal memory is required because the ISR accesses
this ring; no allocation occurs per packet. Normal latency still needs hardware
measurement; DMA priming and network delays affect wall-clock latency.
Overflow beyond either the software-ring or total budget cancels the session. Progress
is sent every four microphone frames (nominally 80 ms) and counts completed DMA
samples, never socket/queue writes. The proxy must limit its outstanding window
to fit this budget and wait for the final played position of an ended epoch
before normally starting the next epoch. A premature normal start fails
explicitly rather than truncating the previous tail. A new epoch immediately
after `playback.clear` is supported even before the clear acknowledgment returns.
On clear, the amplifier
is muted, TX disabled and pending buffers zeroed while RX remains enabled.
The acknowledgment is conservative by at most one incomplete 20 ms DMA block.
Actual shared-clock continuity and acoustic behavior require board testing.
Reconnect never retransmits old microphone audio. Provider failures do not
downgrade to half-duplex or substitute a model.

## Validation status and next gates

See [validation record](docs/validation.md). Compilation and portable tests
passed. BLE/WinRT interoperability, actual flash/PSRAM, LCD jumper state,
microphone channel/gain, speaker polarity/acoustics, SD power interruption,
personal-account consent, OneDrive byte equality and replay/reconciliation,
and fake/live proxy integration have **not** been exercised on hardware.
These are acceptance gates, not claimed successes.
