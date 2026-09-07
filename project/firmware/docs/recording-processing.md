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

## HTTP and UI behavior

The published contract is
[`recording-processing-v1`](../../protocols/recording-processing-v1.md).
Only the origin of configured `wss://<DNS-host>/v1/voice` is reused as HTTPS.
Userinfo, port overrides, queries, fragments, alternate paths, HTTP downgrade,
redirects and arbitrary operation URLs are rejected. The only REST paths are
`/readyz`, `/v1/recordings/status`, and `/v1/recordings/process`.
The owner-approved PoC profile already configures:

`wss://recorder-proxy.grayplant-a3ab794a.eastus2.azurecontainerapps.io/v1/voice`

Each pending file is handled serially:

1. Obtain `identity_access(AUTH_PROXY)`; never forward a Graph bearer to API B.
2. Show **WARMING** and probe `/readyz`, with a 120-second overall warmup budget.
3. Show **CHECKING TRANSCRIPT** and POST status with the durable source identity.
4. Only `not_started` / `retry_required` permits a process POST; show
   **TRANSCRIBING**, with a 210-second device operation budget.
5. Require version 1, a known status, operation ID, and both bounded sidecar IDs
   before durably marking completed.

Operation-specific HTTPS uses asynchronous SDK continuation, short socket
polls, a monotonic deadline and cancellation checks; existing callers retain
their original 10-second timeout and response capacity. Processing request and
response bodies are limited to 4096 bytes. Late/uncertain outcomes retain the
outbox; the next explicit Sync queries status, never blindly reuploads audio.
Numeric Retry-After on processing 429/503 persists a not-before time, including
across reboot. No automatic queue or retry worker runs.

Cancellation, missing API-B consent, malformed responses, timeouts, and provider
errors cannot turn upload success into transcript success. The UI distinguishes
**UPLOADED PROCESS PENDING** and stable error codes (for example
`consent_required`), without erasing Graph credentials. Other recordings can
still upload after a processing failure. Above 30 minutes, PCM WAV upload and
playback remain valid; automatic processing is skipped with a friendly message.
Backend `recording_too_long` receives the same treatment.

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
These mocks do not prove real TLS/SD durability, SHA-1 implementation, acoustic
behavior, backend performance, or stack margins.

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
