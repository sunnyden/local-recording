# Windows BLE recorder companion

Real Bleak/WinRT transport, Espressif protocomm **Security 2 patch 1**
(SRP6a + AES-GCM), standard `network_provisioning` Wi-Fi protobuf endpoints,
and protected `recorder-control` JSON v1. The recorder, not this PC, owns OAuth
device grants, polling, refresh/access tokens and durable account state.

## Install and validate

Run in PowerShell from this directory. Python 3.11+ is required; the complete
Windows dependency lock was resolved and tested on Windows x64/Python 3.14.5.

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements-windows.lock
.\.venv\Scripts\python.exe -m pip install --no-deps --no-build-isolation -e .
.\.venv\Scripts\python.exe -m pytest tests -q
.\.venv\Scripts\python.exe -m recorder_companion --help
```

No ESP-IDF installation, protobuf compiler, SRP package from an unknown publisher,
or network source checkout is needed at runtime. Vendored Espressif implementation
and generated protobuf sources are pinned by commit. See
[`src\recorder_companion\_espressif\NOTICE.md`](src/recorder_companion/_espressif/NOTICE.md)
for repository provenance, licenses, original file hashes and import-only changes.
Runtime dependencies and test/build dependencies are pinned; no moving Git branch
is fetched at install/startup.

## Use

The default firmware intentionally refuses Setup **before BLE advertising or
Wi-Fi initialization** until the owner approves a credential-protection profile
and satisfies the physical LCD/jumper safety gates. Therefore a default build
may expose no discoverable provisioning service at all. This is not a pairing
failure, and the companion cannot bypass those firmware gates. Follow the
firmware's documented approval/safety procedure before attempting these steps.

1. Enter physical **Setup** mode on the recorder. Do not record/play/sync/AI
   simultaneously. Keep the displayed fresh setup username/password private.
2. Enable Windows Bluetooth and use a normal interactive terminal.
3. Discover and select a recorder:

   ```powershell
   .\.venv\Scripts\recorder-setup.exe scan
   .\.venv\Scripts\recorder-setup.exe setup --address "AA:BB:CC:DD:EE:FF"
   ```

   `setup` without `--address` selects only when exactly one recorder is found.
   Names/addresses are discovery hints, not authentication. The default service
   UUID is `1775244d-6b43-439b-877c-060f2d9bed07`, confirmed from the real board
   advertisement and the pinned SDK. Firmware using a custom service
   UUID needs `--service-uuid "<uuid>"` on **both** commands.

4. Enter the setup username and password at hidden prompts. They are never
   accepted as CLI arguments, environment variables or configuration files.
   An unavailable non-echo terminal fails rather than echoing passwords.
5. Use the interactive commands:

   | Command | Behavior |
   | --- | --- |
   | `wifi` | Prompt SSID and hidden Wi-Fi password; set/apply through standard secured `prov-config`, then wait for status. Empty password permits open Wi-Fi. |
   | `status` | Get Wi-Fi status, application status, and separate Graph/proxy authorization status. |
   | `graph` | Start the recorder's Graph device flow, show the public verification URI/code, and poll protected status. |
   | `proxy` | Separately authorize the proxy audience; never reuse a Graph token. |
   | `cancel-graph`, `cancel-proxy` | Stop the recorder's pending grant for that resource. |
   | `unlink` | Require typing `UNLINK` before sending `auth.unlink` with `confirm:true`. |
   | `reconnect` | Prompt displayed setup credentials again and establish a completely fresh Security 2 session. |
   | `finish` | Wait for the `setup.finish` acknowledgement, then disconnect. |
   | `quit` | Disconnect without claiming setup completed; recorder timeout ends setup. |

Wi-Fi connection **does not** finish setup: BLE endpoints must remain live for
both consent flows. If `auth.start` reports pending while the ESP obtains its
device grant, `auth.status` must later include `user_code`, `verification_uri`
and `expires_in`. The PC never receives the polling `device_code`.

Entering a new physical Setup session makes Espressif's manager disconnect and
clear the active RAM Wi-Fi configuration; its saved NVS copy is retained. Run
`wifi` again in that Setup session before `graph`. Normal reboot reconnects
using the saved configuration. A Bluetooth `reconnect` alone does not configure
Wi-Fi. Valid device-level rejections retain the secure session and show a local,
allowlisted explanation; malformed responses still invalidate the connection.
If the resource is already authorized, `auth.start` may return
`state: "authorized"` without starting another grant; the companion reports
success without opening a browser or polling again.

Browser opening requires an explicit `y` at a prompt. Only HTTPS and these
exact host/path pairs are accepted (no query, fragment, user-info or port):

* `microsoft.com/devicelogin`
* `www.microsoft.com/devicelogin`
* `www.microsoft.com/link` (returned by the live consumer device-grant endpoint)
* `login.microsoftonline.com/common/oauth2/deviceauth`

The human signs in and consents in their own browser. The tool neither automates
Microsoft sign-in nor stores Microsoft credentials, cookies or tokens. Opening
the normal browser may of course use that browser's existing sign-in session.

The firmware's public-client registration and API scope belong in firmware/cloud
configuration, not companion arguments. The companion sends only `graph` or
`proxy` as the resource selector. Separate real `/consumers/devicecode` requests
for both configured resources have been accepted in cloud preflight; this proves
neither human consent/token issuance nor BLE interoperability. Do not infer that
an account is authorized from that preflight result.

## Transport, limits and recovery

* Match the configured service UUID; discover endpoint names from GATT
  Characteristic User Description (`0x2901`) descriptors. Required endpoints:
  `proto-ver`, `prov-session`, `prov-config`, `recorder-control`. Duplicate/missing
  descriptors or lack of read/acknowledged-write properties fail closed.
* Read `proto-ver` and require `prov.sec_ver:2` and `prov.sec_patch_ver:1`.
  This metadata is not trusted as authentication: the SRP server proof must
  subsequently verify using the displayed secret. Security 0/1/patch 0 fallback
  is forbidden.
* Firmware has link-layer forced encryption and bonding disabled. The companion
  does not require Windows OS pairing or create a persistent bond as a setup
  prerequisite; application-layer Security 2 is mandatory regardless. This is
  not a plaintext credential fallback.
* Use one acknowledged Bleak `write_gatt_char(..., response=True)` for each
  logical protocomm request and an uncached characteristic read for its reply.
  This follows Espressif's host implementation. Windows handles ATT Prepare/
  Execute Write fragmentation and long reads. **Do not** break an encrypted
  message into independent ordinary characteristic writes; those would be
  interpreted as separate malformed protocomm requests.
* The ATT MTU is not assumed to exceed its default 23 bytes. The SRP3072 initial
  message is approximately 400 bytes and therefore requires working Windows/
  firmware long-value handling. The GATT characteristic-value limit is **512
  bytes**; the application protocol has an independent 4096-byte JSON ceiling.
  For this BLE transport, firmware responses must fit **496 plaintext bytes**
  plus a 16-byte GCM authentication tag. Oversized data is rejected, never
  truncated or silently sent plaintext. No custom fragmentation wire protocol
  is invented. A larger future recorder response needs coordinated protocol/
  firmware changes, not merely a larger receive buffer.
* Encrypt/write/read/decrypt is serialized because patch 1 increments one
  shared nonce counter. Connection/handshake transport failures get at most two
  attempts by default, with a **new** security adapter on every attempt.
  Authentication failure is not retried automatically.
* A timeout, I/O failure or malformed/authentication-failed response discards the
  session. Side-effect commands (Wi-Fi apply, auth start/cancel/unlink, finish)
  are **never replayed**: a lost reply may mean the operation already happened.
  Re-enter Setup if needed, reconnect, check status, and explicitly decide the
  next command. A lost Finish acknowledgement is not reported as success.
  Disconnect failures are explicit redacted errors; when another operation is
  already failing, cleanup failure is attached as a note without replacing that
  primary failure. A failed disconnect stops automatic reconnect attempts.
* Per BLE operation timeout: 20 seconds. Wi-Fi status wait: 90 seconds.
  Auth display/status wait: at most 900 seconds by default, shortened by the
  returned code expiry. This polls the recorder, not Microsoft. Ctrl+C attempts
  auth cancellation when the secure session remains usable, then disconnects.
  If the BLE session is lost, the device's own setup/grant timeout remains
  authoritative.

No payload/debug logging is enabled; server-provided messages, driver exception
details and arbitrary status fields are not printed. Known token/password fields
in JSON replies are rejected even when nested. Setup/Wi-Fi secrets exist only
transiently in process memory. Python immutable strings, the Windows stack, OS
crash dumps and upstream SRP objects cannot promise cryptographic memory wiping.
Do not use screen recording, shell debugging or crash dumps during provisioning.

## Verification and remaining hardware gate

Unit tests exercise default-MTU fragmentation in a **fake OS GATT backend**,
strict response correlation/types, URL allowlisting, redacted errors, delayed
device-code display, Wi-Fi/auth lifecycle, fresh-session retries and no command
replay. Tests also import and execute the **real vendored Espressif SRP client**,
construct its protobuf handshake and reject an invalid server proof.
An in-process cipher self-consistency test verifies patch-1 nonce advancement
and ciphertext replay rejection using the real upstream implementation.
An offline provenance test restores only import spellings and checks every
vendored upstream source against its original SHA-256.

The production path is not a mock: it imports Bleak and the pinned Espressif
Security 2 implementation. However, unit tests do not prove Windows-driver/
firmware interoperability. No real BLE connection/provisioning, Wi-Fi mutation,
Microsoft authorization or physical recorder test was performed during code
implementation. Before release, verify Windows long-write/read behavior at MTU
23, service/descriptor discovery, server proof and patch negotiation, endpoint
survival after Wi-Fi, real Graph/proxy flows, reconnect without reboot, and
Finish response delivery before firmware stops BLE. Fail closed rather than
substituting weaker security if a board/driver cannot support these operations.
