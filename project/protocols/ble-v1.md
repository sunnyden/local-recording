# Recorder BLE provisioning protocol v1

Use Espressif `network_provisioning` with protocomm Security 2 patch 1
(SRP6a/AES-GCM). Require `prov.sec_ver:2` and `prov.sec_patch_ver:1`
metadata and verify the SRP server proof; do not downgrade to older security.
Use its existing BLE transport, service discovery and standard Wi-Fi endpoints,
not a plaintext replacement. A physically initiated setup session displays a
fresh username/setup password on the LCD. The host uses that credential to
authenticate before sending Wi-Fi settings or application commands.

Register the application endpoint `recorder-control` before provisioning starts.
The security layer encrypts/decrypts endpoint payloads. Payloads are UTF-8 JSON,
at most 4096 bytes after decryption at the application layer. The BLE GATT
transport imposes a stricter effective limit of 496 plaintext bytes per
request/response plus the 16-byte GCM tag, fitting a 512-byte characteristic.
Requests have `v:1`, `id` (1-64 ASCII
characters), and `type`. Responses echo `v` and `id`.

| Request type | Fields | Result |
| --- | --- | --- |
| `status` | none | `ok:true`, Wi-Fi/auth state; no tokens or passwords |
| `auth.start` | `resource`: `graph` or `proxy` | `ok:true` with code/URI/expiry, initial `state:pending`, or `state:authorized` when authorization is already available |
| `auth.status` | `resource`: `graph` or `proxy` | `ok:true`, `state`: idle, pending, authorized, denied, expired, or error; pending replies include available `user_code`, `verification_uri`, and remaining `expires_in` |
| `auth.cancel` | `resource`: `graph` or `proxy` | stop the active device grant |
| `auth.unlink` | `confirm:true` | erase saved account credentials and account-bound synchronization state |
| `setup.finish` | none | acknowledge and close provisioning cleanly |

Errors use `ok:false`, stable `code`, and a redacted `message`. Reject unknown
versions, types, resources, excessive sizes and invalid field types. Never
return access/refresh tokens, the polling `device_code`, or upload-session URLs.

The ESP calls Microsoft `/devicecode` and `/token` itself and owns polling,
refresh and durable token storage. The PC displays the verification URI and
user code and may open the user's browser; the user signs in and consents.
The companion polls the protected status endpoint. It must not accept arbitrary
browser URLs from an untrusted BLE advertisement or log setup credentials.

Discover endpoint names using GATT Characteristic User Description descriptors
(`0x2901`). The default provisioning service UUID is
`1775244d-6b43-439b-877c-060f2d9bed07`; any custom UUID must be configured on
both sides. Required endpoints are `proto-ver` (not `prov-ver`),
`prov-session`, `prov-config`, and `recorder-control`. The firmware does not
require OS pairing/bonding: authenticated Security 2 protects the payloads
independently of link pairing. The host must not make OS pairing mandatory.
Use one acknowledged characteristic write and an uncached read per
logical protected request. Windows/ATT handles Prepare/Execute Write and long
reads; do not split encrypted data into independent ordinary GATT writes.
Validate long-value handling at the default MTU on the actual board/driver.
Serialize protected requests because Security 2 patch 1 advances a shared
nonce counter. After uncertain delivery, discard the session and reconnect
with a fresh handshake; do not automatically replay side-effecting commands.

Wi-Fi and application authentication may fail independently. Provisioning must
remain open after Wi-Fi connects so the application endpoints remain usable.
On `setup.finish`, keep the acknowledgment readable before deferred BLE
shutdown. A lost acknowledgment is not success. Timeout, cancellation and
explicit completion end the session. Reopening setup
must work without a reboot. Reconnect requires a fresh protected session.
