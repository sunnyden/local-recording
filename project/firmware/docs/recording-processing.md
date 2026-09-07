# Timestamped recordings and synchronous processing

The owner confirmed the pre-change record/play/Sync/AI baseline. This extension
does not change `recorder.voice.v1`, the 16000-sample playback capacity, 50-frame
PSRAM voice microphone queue, codec settings, or credential provisioning.

## Names and capture time

New captures use `AudioRecording_YYYYMMDD_HHMMSS.wav` with a fixed timezone
configured by `CONFIG_RECORDER_TIMEZONE_OFFSET_MINUTES` (east-positive minutes,
default `480`, UTC+08:00; range -720 to 840). The UTC system clock is unchanged.
The snapshot is taken before recording starts, not during upload.

Same-second/backward-clock collisions reserve `.part` exclusively and use
`_001` through `_999`. Existing WAV, partial, checkpoint, and metadata names
reserve the stem. Exhaustion fails visibly, never overwrites.
Before a valid UTC clock is available, recording remains usable with
`AudioRecording_UNTIMED_<16 random hex digits>.wav`. No later automatic rename
or fabricated timestamp occurs. Existing `rec-*.wav` and `rec-*.part` remain
accepted by playback, catalog, recovery and Sync.

A small, fsynced `<stem>.meta` stores version, clock-valid flag, start UTC seconds,
and the capture's effective offset. It survives finalization/recovery and later
timezone changes. Missing legacy metadata or invalid metadata omits the optional
`recorded_at` hint; invalid metadata is logged without its contents. The existing
alternating 16-byte `.ckp` format is unchanged. Metadata and jobs never appear in
the playable catalog.

## Upload receipt and outbox

Each successfully uploaded WAV gets an SD `<stem>.job` containing only nonsecrets:
drive ID, item ID, SHA-1, source size, name, optional RFC3339 capture timestamp,
processing state, retry-not-before, and confirmed sidecar IDs. Two checksummed
slots retain the preceding state if an update tears. A completed slot is kept as
a receipt rather than deleting the file and accidentally re-notifying on every
Sync. No tokens, upload capability URLs, transcript text, or refresh credentials
are stored here.

Fresh uploads, same-name/size/SHA-1 remote matches, and reconciled lost final
responses all use this path. The previous single `last_sync` NVS record is no
longer used for completion. Existing local files without receipts can notify
processing after remote hash verification.

On each explicit Sync, the currently authenticated Graph drive is resolved
first. Its durable jobs are processed before scanning WAVs, including jobs whose
local WAV is absent. Matching receipts avoid WAV reupload. Jobs bound to another
drive are never submitted; changing accounts or mutating a receipted WAV fails
visibly rather than silently transferring ownership. The backend additionally
authorizes the source with the API-B user's OBO Graph identity.

The SD card is not an authorization boundary. CRCs detect torn/corrupt records,
not malicious changes. If both slots are corrupt or SD persistence fails, Sync
stops visibly and retains the WAV; do not delete these files as an automatic
repair. Lost SD data requires operator-controlled reconciliation.

## Processing stream, status, and UI behavior

The published contract is
[`recording-processing-v1`](../../protocols/recording-processing-v1.md).
Only the origin of configured `wss://<DNS-host>/v1/voice` is reused as HTTPS/WSS.
Userinfo, port overrides, queries, fragments, alternate paths, HTTP downgrade,
redirects and arbitrary operation URLs are rejected. The only REST paths are
`/readyz`, `/v1/recordings/status`, and `/v1/recordings/process`.
The preferred processing transport is the separate
`wss://<same-host>/v1/recordings/process-stream` socket with subprotocol
`recorder.processing.v1`. It never shares the microphone, audio queues, or voice
connection. The server's legacy process HTTP endpoint remains supported for
other clients; firmware does not silently fall back to it.
The owner-approved PoC profile already configures:

`wss://recorder-proxy.grayplant-a3ab794a.eastus2.azurecontainerapps.io/v1/voice`

Each pending file is handled serially:

1. Obtain `identity_access(AUTH_PROXY)`; never forward a Graph bearer to API B.
2. Show **WARMING** and probe `/readyz`, with a 120-second overall warmup budget.
3. Show **CHECKING TRANSCRIPT** and POST status with the durable source identity.
   The device deadline is **45 seconds**, exceeding the backend's 30-second
   status budget (the former 10-second setting could report failure after
   sidecars had actually been created).
4. Only `not_started` / `retry_required` permits a new processing stream.
   Force-refresh the API-B token before the authenticated upgrade, then send
   exactly one text message containing the same v1 source identity.
5. Require version 1, a known status, the operation ID confirmed by status and
   the stream's `started` message, and both bounded sidecar IDs
   before durably marking completed.

The socket has a 30-second connection/application-message liveness budget, not
a 210-second total deadline. Valid `started`, progress, heartbeat and terminal
messages update liveness. Proxy heartbeats at most 10 seconds apart keep opaque
Speech processing alive; WebSocket ping/pong alone does not substitute for
application liveness. A host test completes after 250 seconds of heartbeats.
Authentication expiry and upstream safety timeouts can still terminate work.
KEY2 sends a bounded cancel message and destroys this request-owned connection;
there is no detached job or queue worker.

The display follows **RESOLVING**, **DOWNLOADING**, **VALIDATING**,
**TRANSCRIBING**, **SAVING TRANSCRIPT**, and **VERIFYING**. Percentages appear
only when the backend supplies actual completed/total byte counts. A heartbeat
does not imply incremental Speech percentage, and the completed WAV upload's
100% is not displayed as transcription progress.
When the backend supplies WAV-to-Speech upload byte counts in the `transcribing`
phase, the label is **SPEECH UPLOAD**, not **TRANSCRIBING xx%**. Phase-only
heartbeats return to **TRANSCRIBING** without a percentage.

Malformed fragments, embedded/escaped NUL, duplicate JSON members, binary/audio
messages, unknown phases, mismatched operation IDs, duplicate terminal results,
and missing/oversized sidecar IDs cannot mark a receipt complete. Message
assembly uses one bounded 4096-byte buffer, with no per-file audio allocation.
Callback code never calls WebSocket send/stop or writes SD; the Sync task owns
request sending, cancellation, cleanup and durable completion.

Operation-specific HTTPS uses asynchronous SDK continuation, short socket
polls, a monotonic deadline and cancellation checks; existing callers retain
their original 10-second timeout and response capacity. Processing messages and
status bodies are limited to 4096 bytes. After uncertain stream delivery, the
device makes at most three status-only reconciliation calls; a still-active
operation remains **PROCESSING / CHECKING / UPLOADED PROCESS PENDING**, not a
claim of failed WAV upload or a false hard transcription deadline. This Sync
never sends a second processing start or an HTTP fallback replay. A later
explicit Sync can retry the retained receipt after checking status again.
Numeric Retry-After on status 429/503 persists a not-before time across reboot.

Cancellation, missing API-B consent, malformed responses, timeouts, and provider
errors cannot turn upload success into transcript success. The UI distinguishes
**UPLOADED PROCESS PENDING** and stable error codes (for example
`consent_required`), without erasing Graph credentials. Other recordings can
still upload after a processing failure. Above 30 minutes, PCM WAV upload and
playback remain valid; automatic processing is skipped with a friendly message.
Backend `recording_too_long` receives the same treatment.

Already completed receipts remain skipped. For pending transcription, a proxy
`source_not_found` is not sufficient to declare the WAV deleted: the device
checks the exact uploaded item ID directly with its current drive's Graph
authorization. Only a successful HTTP 404 on that check saves the terminal
`PROCESS_REMOTE_MISSING` receipt. Later Sync operations skip both reupload and
processing for that item, even across reboot, while retaining the SD WAV.
Authentication failures, network errors, and a missing backend folder while
the source item still exists remain pending rather than silently discarded.
The screen reports **DELETED REMOTE SKIPPED**. Pending error codes now remain
visible while other recordings continue syncing, not just at the end.

## Validation and hardware gate

Run from `project\firmware` using the existing Zig installation:

```powershell
.\tools\test-core.ps1
.\tools\test-intelligence.ps1
```

The second script runs actual filename/storage/checkpoint/outbox C against a
project-local host filesystem, then actual cloud Sync/processing/UI sources with
mock Graph, API-B, task scheduling and hashing boundaries. Cases cover all
upload-success paths, multiple durable jobs, capture metadata, cancelled or
uncertain processing, malformed/versioned responses, account binding, long WAVs,
Retry-After, and replay without a WAV. The first script retains the existing
audio/voice/identity/upload suite and extends actual HTTPS deadline/body tests.
The stream-specific executable tests the actual WebSocket client with network
fragmentation, 250-second heartbeats, cancellation/stalls, ping-only liveness,
strict terminal/ID validation and bounded malformed input. The integrated cloud
test retains the parent's exact-Graph-404 deleted-source skip cases.
These mocks do not prove real TLS/SD durability, SHA-1 implementation, acoustic
behavior, backend performance, or stack margins.

Stream implementation validation: both host scripts pass, including the
unchanged voice/audio suite, parent deleted-source cases, 45-second status
budget, durable completion after uncertain stream delivery, and 27 transport
scenarios. Both isolated IDF 6.1 configurations built with `ninja -j1`.
Final application artifacts (both below the installed 3 MiB capacity):

| Artifact under `project\firmware` | Bytes | SHA-256 |
| --- | ---: | --- |
| `build-intelligence-poc\embedded_recorder.bin` | 1,535,104 | `2ffbbbb78ff6a6fbf2ba6e056729ef92f70010377deccd86c0c9f270a580409d` |
| `build-intelligence-safe\embedded_recorder.bin` | 1,510,912 | `9a51377bed990a0009600619aa80e2b69ed57524aed6c2da534590066024d97d` |

Build in isolated directories using the installed IDF profile, not the generic
export script. Do not reuse or rewrite the parent's generated sdkconfig.

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py -B build-intelligence-safe -D SDKCONFIG=build-intelligence-safe\sdkconfig `
  -D SDKCONFIG_DEFAULTS=sdkconfig.defaults reconfigure
ninja -C build-intelligence-safe -j4
idf.py -B build-intelligence-poc -D SDKCONFIG=build-intelligence-poc\sdkconfig `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.poc-sync.defaults" reconfigure
ninja -C build-intelligence-poc -j4
```

If the local GCC reports a transient internal compiler error, resume that build
with `ninja -C <same-build-directory> -j1`; do not change firmware logic to work
around a compiler crash. Set the timezone through menuconfig only for the
isolated configuration or in an owner-approved defaults overlay.

**Validation builds are not flash authorization.** Their generated partition
table must not be used on the existing device: preserve its 24 KiB NVS and
factory application at `0x10000`, size 3 MiB. The parent coordinates any later
app-only flash with the existing partition layout. No flash, erase, reset,
eFuse, or credential read is part of this change.

Required board acceptance: capture with/without valid time, same-second captures,
old partial recovery, power interruption during outbox update, real upload plus
both sidecars, proxy cold start, cancel/timeout followed by status reconciliation,
missing B consent while direct upload still works, and unchanged playback/AI.
The parent reports real JSON/TXT sidecars worked on the earlier HTTP firmware.
That result is not acceptance of this new stream transport: live stream phase
updates, long Speech liveness, cancellation/reconciliation and unchanged voice
remain parent-coordinated hardware tests. Do not assume COM3; the parent reports
native USB COM4 and owns any later device operations.

## Voice STOPPING investigation

A reproducible existing firmware gap was found after the reported real-device
failure: receiving `state:"stopping"` only changed the display. If the following
`error`, `stop`, or disconnect was lost, microphone transmission could continue
until the session limit. Firmware now stops capture on that notice and allows
a 250 ms grace period for the existing error/stop frames, then initiates cleanup.
An RFC6455 close frame also initiates cleanup without waiting for a separate
disconnect event. No voice wire messages, buffer sizes, codec or audio timing
settings changed.

The actual host voice client tests pass stopping-notice-only, a following error,
close-without-disconnect, and session-allocation failure. Compiling the original
HEAD voice source against the new stopping-only test fails as expected; the
patched source passes. A native-only null dereference in allocation-failure
cleanup was also guarded. Existing voice/audio fixtures remain passing.
Additional deterministic interleavings pass cancellation inside `ready`/audio
startup, cancellation while waiting without provider audio, a late `ready`
callback during socket stop, and assertions that socket callback/task cleanup
precedes audio/queue release and that a new session cannot start during cleanup.
These tests exercise the actual voice client against boundary mocks; they do
not reproduce ESP cross-core scheduling, a late I2S read, or SDK internal locks.

**This does not prove the cause of the observed device hang, nor impose a hard
deadline on SDK teardown.** The SDK's `esp_websocket_client_stop` / `destroy`
wait for its task with `portMAX_DELAY`; even its nominally timed close API falls
back to an unbounded wait. No safe forced task deletion/reset was introduced.
Nonsecret cleanup-stage logs now distinguish microphone quiesce, WebSocket
stop/destroy, audio stop and cleanup completion for the next authorized test.
Local I2S reads/writes use 200 ms I/O timeouts and the board's I2C paths are
bounded; no local-recording lock cycle was demonstrated. Passive device logs
and parent-coordinated hardware testing are still needed to distinguish a
missing terminal notification from an SDK/driver teardown stall.
The parent's later evidence describes one occurrence followed by a user reset
and recovery, with server `voice_session_end code=normal`. That log does not
prove which firmware cleanup stage stalled. No additional firmware
callback/task/audio lock cycle was reproduced, and no speculative buffer,
priority, transport-timeout, or vendor-SDK rewrite was made.
