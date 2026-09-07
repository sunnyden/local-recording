# Intelligent recording

This extends recording and OneDrive upload with synchronous transcription and
read-only voice retrieval. It does not change `recorder.voice.v1`, the
16000-sample playback limit, or the 8000-sample startup prefill.

## Filenames and time

New recordings use `AudioRecording_YYYYMMDD_HHMMSS.wav` in the configured
timezone, initially UTC+08:00. Same-second collisions get `_001`, `_002`, etc.
An unsynchronized clock produces `AudioRecording_UNTIMED_<id>.wav`; recording
does not depend on network availability. Existing `rec-*.wav` files remain
usable. Timestamp provenance is local recording metadata, not a timestamp
invented at upload time.

## Upload and processing

The ESP uploads the WAV directly to OneDrive as before, then records a
nonsecret processing notification on SD. Every upload-success path, including
reconciliation with an already uploaded file, follows this path.

Processing uses separate API-B-authenticated endpoints documented in
[`recording-processing-v1.md`](../protocols/recording-processing-v1.md).
The proxy checks the caller, source item, allowed folder, content fingerprint,
WAV format and duration before submitting audio to Fast Transcription.
Graph access tokens are not accepted as API-B authentication.

The backend processes one recording in the connected request. There is no
cloud queue, scheduled processor or separate transcription worker. The SD
outbox is a device-side retry journal, not a server queue. Cold-start warmup
is bounded at 120 seconds.

The processing WebSocket at `/v1/recordings/process-stream` uses the separate
`recorder.processing.v1` subprotocol. It reports phases and measurable file
transfer progress while keeping the connection alive during opaque Speech
work. Heartbeats indicate proxy liveness, not transcription percentage.
It is not subject to the old HTTP operation's 180/210-second budgets.
Authentication expiry, cancellation, connection loss and upstream safety
timeouts still apply; processing does not detach after the socket disconnects.
Streamed Speech calls allow a 15-minute response-read stall timeout because
that API does not supply intermediate progress. File-transfer activity is
measured separately. Repeated cancellation waits for outstanding filesystem
work and spool cleanup before releasing the processing slot.
The original HTTP process/status endpoints remain available. Status checks
allow 45 seconds on the ESP, above the backend's 30-second status budget.

Processing may be incomplete even when upload succeeded. Uncertain results
are reconciled through status, not by blindly starting transcription again
or uploading the same WAV. The display distinguishes processing, checking
the result and pending work from a failed WAV upload.

Deleting a previously uploaded WAV in OneDrive does not cause it to be
reuploaded. For pending transcription, Sync confirms the exact item's absence
directly with Graph before recording a durable deleted-remote skip. Subsequent
Sync operations leave it skipped and keep the local WAV. A proxy-folder,
permission or network failure alone cannot create that skip.

Automatic transcription is limited to 30 minutes of PCM16 mono 16 kHz audio.
Longer recordings still upload and play normally. The legacy HTTP budget is
not a guarantee that every 30-minute recording completes before ingress timeout.
Do not retry an uncertain operation using a new source identity.

## Outputs

For `AudioRecording_20260907_220536.wav`, the backend writes:

```text
AudioRecording_20260907_220536.txt
AudioRecording_20260907_220536.transcription.json
```

TXT is readable text. JSON records source identity, processing version,
timestamp provenance, detected locales, text and provider timing/result
metadata. An explicit valid no-speech result is allowed; transport or
authentication errors are not converted to empty transcripts.

Completion means both sidecars are confirmed. A conflicting/manual file is
not silently overwritten. The WAV is never modified or deleted. A crash
before results are durably written can require another Speech invocation;
exactly-once transcription billing is not promised.

These are ordinary OneDrive files. No undocumented VROOM transcript API is
used, and native OneDrive player captions/transcript-pane integration is not
provided.

## Existing Foundry resource, secretless Graph access

Both APIs use the same Foundry AIServices account:

| API | Resource endpoint |
| --- | --- |
| Voice Live | `https://recorder-ai-das7i6efgflpa.services.ai.azure.com` |
| Fast Transcription | `https://recorder-ai-das7i6efgflpa.cognitiveservices.azure.com` |

Fast Transcription uses `/speechtotext/transcriptions:transcribe` with
`api-version=2025-10-15`, a managed-identity bearer token and `locales: []`
for the service's supported multilingual model. A Foundry project URL is not
the direct Speech REST URL. No standalone Speech resource or key is needed.

API B exchanges the authenticated user's API-B token for delegated Graph
access using OBO. Its confidential-client credential is an assertion from
the existing runtime managed identity, trusted by B's federated identity
credential. The runtime assertion and user assertion are separate tokens.
The backend keeps downstream tokens only in bounded process memory.

Bootstrap with `infra\scripts\Configure-Obo.ps1`, preview first, then `-Apply`.
This merges B's delegated `Files.ReadWrite`, adds A to B's
`knownClientApplications`, and configures runtime identity federation.
Fresh combined user consent can be necessary. This is an operator step:
the normal GitHub deployment identity does not have directory-admin powers.

The actual personal-account OBO diagnostic succeeded with Graph read/write
and removal of its synthetic probe file. Repeated immediate OBO exchanges
also succeeded; that is not an elapsed-token-expiry test. A separate bounded
managed-identity Speech diagnostic accepted one second of synthetic silence
using the same Foundry account and existing roles. Neither diagnostic alone
establishes recorded-speech accuracy or complete device-to-transcript acceptance.

## Voice retrieval and rollout

The model has only `search_onedrive`, `get_onedrive_item` and
`read_onedrive_text`. The configured scope initially contains
`local-recording` and its descendants. Each fetched item is checked against
the scope; OAuth `Files.ReadWrite` itself is not folder-limited.

The agent searches and reads evidence before answering stored-information
questions, treats retrieved content as data rather than instructions, and
cites filenames/timestamps when available. It has no write tool or implicit
live-weather/web access. Graph indexing can lag; retrieval is not vector RAG.

Read tools use folder-scoped Graph search plus a bounded recent-transcript
fallback for indexing lag. Tool failures are returned as explicit model-readable
results. Tool-call arguments, recording contents and preauthenticated download
URLs are not logged. Lifecycle diagnostics distinguish sending a result,
provider acknowledgment and requesting the continuation. Interruption invalidates
the old turn's work rather than allowing it to resume a newer turn.

Both backend features default off for staged rollout:

| Setting | Initial value |
| --- | --- |
| `RECORDING_PROCESSING_ENABLED` | `false` |
| `ONEDRIVE_TOOLS_ENABLED` | `false` |
| `GRAPH_ROOT_PATH` | `local-recording` |
| `SPEECH_ENDPOINT` | The existing Foundry Cognitive Services origin above |

Enable processing and retrieval only after their auth/configuration gates are
met. The GitHub production environment carries these nonsecret settings.
`New-ProxyConfiguration.ps1 -EnableRecordingProcessing -EnableOneDriveTools`
generates equivalent local deployment configuration. Disabled processing
does not invalidate successful OneDrive uploads; notifications remain pending.

Final hardware acceptance includes recording with a known clock, Sync from a
cold backend, both sidecars, and a spoken question grounded in that transcript,
plus ordinary multi-turn conversation and interruption.
