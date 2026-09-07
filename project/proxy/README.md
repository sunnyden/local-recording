# Authenticated recorder voice proxy

Python 3.12+ ASGI application for `..\protocols\voice-v1.md`. It forwards **raw
16 kHz mono PCM16 microphone audio** only after API-B access-token validation.
No credentials, recording files, transcripts, audio, or request headers are
logged by this application. Do not enable HTTP/WebSocket/identity debug logging.
Ingress must terminate trusted TLS; the production device uses WSS only.

## Explicit deployment configuration

Required nonsecret environment variables:

| Name | Value |
| --- | --- |
| `API_B_CLIENT_ID` | B's application/client UUID; v2 token `aud`, **not** `api://...` |
| `PUBLIC_CLIENT_A_ID` | Authorized public client A's UUID; validated token `azp` |
| `ALLOWED_USER_OID` | Explicitly enrolled consumer user's validated `oid` UUID |
| `AZURE_CLIENT_ID` | User-assigned managed identity client UUID |
| `VOICELIVE_ENDPOINT` | Resource HTTPS origin, e.g. `https://RESOURCE.services.ai.azure.com` |
| `VOICELIVE_MODEL` | Explicit model or BYOM deployment name; initial target `gpt-realtime-2` |
| `VOICELIVE_PROFILE` | `native` or `byom-azure-openai-realtime` |

Optional `MAX_SESSION_SECONDS` is 1-900 (default 900).
`VOICELIVE_API_VERSION`, if set, must be exactly **2026-07-15**.
Missing identity, allowlist, model, profile, or endpoint settings fail startup.
There is no first-user enrollment, API-key fallback, developer credential-chain
fallback, fake-provider environment flag, or unauthenticated production mode.
BYOM requires an existing correctly configured deployment and RBAC; this code
does not create resources or substitute another model when one is unavailable.

Authentication is pinned to Microsoft's public-cloud **consumer** OIDC
discovery endpoint and consumer tenant
`9188040d-6c67-4c5b-b112-36a304b66dad`. The actual accepted issuer is the
tenant-GUID `https://login.microsoftonline.com/<consumer-tenant>/v2.0`, not
the literal `consumers` alias. Only trusted common/consumer signing-key
endpoints, RS256, delegated `access_as_user`, A's `azp`, and the configured
stable consumer `oid` are accepted. Graph tokens, ID tokens, app-only tokens,
other tenants/users/clients, expired tokens, token-directed keys, and arbitrary
issuer/JWKS overrides are rejected. Key refresh is cached for one hour and
rate-limited to once per 30 seconds, including unknown-kid attempts. Issuer,
audience and signature checks are never disabled on failure. Use separate Graph
tokens for direct upload only on the device; never send refresh tokens to this
service. Optional server-side Graph access exchanges the validated API-B token
through OBO; it never treats a Graph token as API-B authentication.

## Run and validate (PowerShell, from repository root)

```powershell
python -m venv project\proxy\.venv
project\proxy\.venv\Scripts\python.exe -m pip install -r project\proxy\requirements-test.lock
project\proxy\.venv\Scripts\python.exe -m pytest project\proxy\tests -q
# Validate the shared firmware DMA/played-progress timing budget too.
project\proxy\.venv\Scripts\python.exe -m pytest project\tests\test_audio_budget.py -q

# Set every required deployment environment variable above first.
$env:PYTHONPATH = (Resolve-Path project\proxy\src).Path
project\proxy\.venv\Scripts\python.exe -m uvicorn recorder_proxy.app:create_app `
  --factory --host 127.0.0.1 --port 8000 --workers 1 --ws websockets `
  --ws-max-size 4096 --ws-max-queue 4 --ws-ping-interval 20 `
  --ws-ping-timeout 10 --ws-per-message-deflate false --no-access-log
```

Local production runs require a reachable managed-identity endpoint; tests use
Python dependency injection and synthetic RSA-signed JWTs, never real accounts.
The CLI limits above are part of the deployment contract (ASGI receives already
reassembled messages). Do not replace them with unbounded server defaults.

Build the container from `project\proxy` as the Docker context. It uses pinned
Python/runtime packages, nonroot UID 65532, one worker, no build credentials,
port **8000**, and a `/readyz` health check. `/healthz` is process liveness;
`/readyz` indicates startup trust-metadata initialization and acceptance state.
Readiness does **not** claim model quota, managed-identity authorization, or
audio generation has been live-tested. Startup fails closed if trust metadata
cannot be fetched. Health requests never open a billable Voice Live connection.
Use HTTPS-only ingress and disable external request/header/body logging.

## Voice Live boundary

Audio ingestion and playback draining run independently. One response keeps one
device epoch, with a separate ordered queue for each `(item_id, content_index)`.
Repeated deltas continue the same resampler. Later items may arrive before earlier
items finish, including late audio tails for earlier items; they remain buffered
until the earlier item's actual `audio.done` event. A response completion flushes
any remaining tail. Item switches alone never finalize audio.

Pending item PCM is bounded to 2 MiB per session and 128 audio segments per
response, in addition to the existing device and outbound queue limits. Exceeding
these bounds returns an explicit error rather than dropping or reordering audio.
On interruption, played samples map back to the affected provider item; later
unheard segments are truncated to zero. The device's one-second capacity and
500 ms prefill are unchanged.

`providers.py` implements an explicit bounded **raw Voice Live WebSocket**
adapter, not the Voice Live SDK and not the OpenAI GA Realtime schema.
It connects to `/voice-live/realtime?api-version=2026-07-15&model=...`,
adding `profile=byom-azure-openai-realtime` only when explicitly selected.
Async `ManagedIdentityCredential` requests `https://ai.azure.com/.default`.

The session requests `pcm16` input with `input_audio_sampling_rate=16000`,
`server_echo_cancellation`, `azure_semantic_vad_multilingual`, and
service-owned turn detection/interruption, with `create_response: false`.
After the service commits a user turn, the proxy creates its response only
after outstanding clears have final device acknowledgments and their
truncations have been sent (and any canceled response has completed).
By default, `tools: []` and `tool_choice: none` are explicit; incoming function
calls are fatal, not executed. The optional read-only tool rollout is described
below. The exact model and required audio/VAD/AEC/tool settings must be
confirmed in **`session.updated` before `ready`**.
The observed native `gpt-realtime-2` response name
`gpt-realtime-2-global-standard` is an explicitly accepted canonical alias,
not a fallback request. No prefixes, mini models, or other variants match.
BYOM requires the exact configured deployment name and never uses this alias.
AEC verification allows extra service defaults, but its type must remain
`server_echo_cancellation`; an explicit reference must be `server` and explicit
channels must be integer `1`. Client-reference or two-channel responses fail.

This adapter deliberately chooses the documented `pcm16` **24 kHz** output
rather than guessing an unsupported 16 kHz format name. Stateful libsoxr HQ
anti-aliased streaming conversion keeps the device wire at 16 kHz and flushes
short tails on audio/response completion. The audio boundary also accepts
16 kHz providers without resampling. There is no local/proxy AEC or VAD.
Any additional service output format requires an explicit adapter update and
configuration-acceptance test; there is no silent format/model fallback.

## Bounds, interruption, and operational limits

* Only one active session **per process**; HTTP 429 precedes a second upgrade.
  Deploy exactly one worker and one replica. This is not a distributed lease:
  overlapping revisions, multiple replicas, or duplicate deployments can each
  admit a session. Prevent rollout overlap operationally before claiming a
  global one-session billing cap; no hidden storage/lock resource is provisioned.
* Expiry of the authenticated user's token and the 15-minute cap are enforced
  while live, not just on connect. Reconnection is a fresh conversation; no
  old microphone samples or credentials are retained.
* Device messages: at most 4096 UTF-8 control bytes or 664 binary bytes, strict
  header/sequence/sample-position checks. WebSocket transport queues: 4.
* Microphone queue: 25 frames (16 KB PCM), maximum residence/wait 500 ms.
  Outbound queue: 35 records (at most 22,400 PCM bytes), maximum residence/wait
  3 seconds. It remains memory bounded while tolerating provider bursts and
  board playback pacing.
  Service receive: 128 KiB message, 4 WebSocket frames, 64 KiB decoded
  PCM per event; one decoded/resampled chunk is held outside these queues.
  These bounds deliberately stop overloaded sessions instead of accumulating
  arbitrary delayed audio. The 32 most recent interrupted response IDs are
  retained to discard late deltas; older unexpected IDs fail closed.
* No more than 16000 samples (one second, 50 nominal frames) are sent but
  unreported as played. The proxy initially prefills 500 ms, then paces at
  16 kHz with no more than 60 ms catch-up. This covers Wi-Fi jitter, the
  four-period DMA residence, and the firmware's 80 ms progress batching.
  Device progress must report **actual speaker-consumed** samples every four
  microphone frames (nominally 80 ms), plus final drain. The DMA-budget contract
  depends on this cadence. End-of-stream is not proof of playback.
  A new normal playback epoch waits for the previous one to drain. At most
  four stream bookkeeping records and four retired acknowledgment positions are
  retained, not four concurrently playing wire epochs. Normal wire playback is
  strictly sequential; only explicit clear invalidates a previous epoch.
* Service speech-start invalidates old epochs immediately. One serialized
  socket writer drops queued obsolete audio and sends `playback.clear`.
  A late old epoch never follows its clear on the wire. The final
  `playback.cleared` acknowledgment - not bytes sent, queue depth, nor an earlier
  progress report - maps to `conversation.item.truncate.audio_end_ms`.
  Server `interrupt_response` owns generation cancellation; the proxy does not
  race it with a second cancellation of an already-completed response.
  Missing clear acknowledgment is fatal after 2 seconds. New response creation
  is gated on truncation rather than racing an automatic service response.
  A later audio item can wait up to 15 seconds for the prior item to finish
  actual playback; no-progress credit still fails after the shorter 3-second
  bound.
  A response-created event racing a new speech-start fails closed with
  `interruption_race` rather than emitting unwanted audio.
* Separate microphone/provider/read/write tasks preserve full duplex.
  Network failure, stop, disconnect, timeout, cancellation, provider error,
  malformed controls, backlog, and playback stalls release provider credentials,
  sockets, session slot and tasks. Device errors use stable redacted codes.

Live native diagnostics using an existing Azure CLI identity accepted this exact
proxy configuration, including manual response creation and the canonical
model name. A synthetic text item asking for one word, followed by the unchanged
production `respond()` method, completed through the real event mapper and
resampler: 21,600 samples at 24 kHz became 14,400 samples at 16 kHz. No user
microphone audio was sent and no generated audio was persisted. Azure CLI was
injected into the direct diagnostic only; production authentication was unchanged.

## Optional recording intelligence

The authoritative HTTP contract is `..\protocols\recording-processing-v1.md`.
The ESP audio/control protocol and its queue, playback epoch, and truncation
semantics are unchanged. Both new features are **off by default** so deployment
can enable processing independently of voice retrieval.

| Environment variable | Default / requirement |
| --- | --- |
| `RECORDING_PROCESSING_ENABLED` | `false`; set `true` for synchronous HTTP processing |
| `ONEDRIVE_TOOLS_ENABLED` | `false`; separately enable the three voice read tools |
| `GRAPH_ROOT_PATH` | `local-recording`; operator-configured relative folder, up to eight segments |
| `SPEECH_ENDPOINT` | Required when processing is enabled; existing resource's HTTPS custom origin, preferably `https://RESOURCE.cognitiveservices.azure.com` |

Flags accept only `true`/`false`. Speech origins must end in
`.cognitiveservices.azure.com` or `.services.ai.azure.com`, with no credentials,
path, query, alternate port, or fragment. The API is pinned to
`/speechtotext/transcriptions:transcribe?api-version=2025-10-15`. An empty
multipart `definition` (`{"locales":[]}`) deliberately selects automatic
multilingual model, not a fixed candidate-language list. The audio is streamed
as multipart data; arbitrary public audio URLs and batch/LLM transcription
fallbacks are not supported. Speech uses the existing async managed identity
with scope `https://cognitiveservices.azure.com/.default`.

`GRAPH_ROOT_PATH` is the canonical deployment name. The initial
`ONEDRIVE_ALLOWED_ROOT` name is accepted as a compatibility alias; setting both
to different values fails startup rather than silently selecting another scope.

### Delegated identity and storage boundary

API B must have delegated Graph `Files.ReadWrite` consent, combined consent
configured for client A, and a federated credential trusting the workload's
managed identity. The federation issuer is the **application/MI home tenant**,
the subject is the MI **principal/object ID**, and its audience is
`api://AzureADTokenExchange`. Those are deployment prerequisites, not inferred
from user input or created by this application.

`GraphTokens` uses asynchronous HTTPx OBO requests against the fixed, validated
consumer tenant authority. It requests a fresh MI credential
(`api://AzureADTokenExchange/.default`) for each exchange and supplies that token
as `client_assertion`, separately from the incoming API-B user `assertion`.
There is no client secret, synchronous MSAL call, refresh-token persistence,
database, queue, worker service, or durable user-token store. At most eight
Graph access-token cache entries exist in process memory, keyed by validated
owner and incoming-assertion digest. Graph 401 triggers one renewed exchange;
user-token expiry still limits authorization.

Graph access resolves the authenticated user's personal drive and the configured
root, then checks every item's actual parent-ID ancestry (maximum 32 hops).
A textual path prefix is never authorization. Cross-drive references, remote
items/shortcuts, symbolic-link facets, packages, arbitrary request URLs, and
pagination outside the authorized collection are rejected. Downloads follow at
most three redirects to approved personal OneDrive HTTPS download domains,
including the exact `my.microsoftpersonalcontent.com` host used by current
personal OneDrive downloads, and never forward a Graph bearer to the download
host. Sibling and lookalike domains are not implicitly allowed. Expanding the configured
root expands what the voice tools can read: treat it as a privileged change.

### Synchronous processing and recovery

`POST /v1/recordings/process` and `POST /v1/recordings/status` require the same
validated API-B bearer as voice. JSON is limited to 4096 UTF-8 bytes:

```json
{
  "v": 1,
  "drive_id": "GRAPH_DRIVE_ID",
  "item_id": "GRAPH_ITEM_ID",
  "source_sha1": "0123456789012345678901234567890123456789",
  "source_size": 320044,
  "recorded_at": "2026-09-07T22:05:36+08:00"
}
```

`recorded_at` is optional, timezone-qualified, and labeled as a device hint in
the sidecar. Unknown fields, caller-selected options/output paths/URLs, duplicate
JSON keys, and alternate versions are rejected. The operation ID is derived
from validated owner, drive, item, SHA1, and fixed pipeline version/options.

Processing is limited to one in-flight operation per process, **without a
queue**; duplicates receive `processing_in_progress`, unrelated contention
receives `busy`. This slot is independent of voice. HTTP handlers themselves
are bounded to eight concurrent requests. Keep one worker and one replica;
this is not a cross-replica lock.

The server verifies Graph size/hash metadata, downloads to a private random
project-working-directory WAV, hashes the actual content, and parses the RIFF
chunks. Only PCM16, mono, 16 kHz WAV with actual duration at most 1800 seconds
is accepted; maximum file size is 57,665,536 bytes including a bounded header
allowance. File reads/writes and WAV parsing use bounded background thread
operations, not the voice event loop. Speech requests are asynchronous.
The entire processing budget is 180 seconds, including Graph and output writes.
Timeout/disconnect/cancellation cancels the operation and removes its WAV.
The container runs in a private writable `/app/data` directory; hard container
termination relies on ephemeral container-storage disposal. No token or
transcript is written to the local spool.

Outputs beside the input WAV:

* `<stem>.txt`: readable timestamped multilingual phrases, source/operation
  identity, and a content checksum. Silence explicitly says “No speech recognized.”
* `<stem>.transcription.json`: completion marker, source SHA1/ETag, pipeline
  metadata, phrase offsets/locales, readable text, and the TXT ID/checksum.

The WAV is never modified. Uploads are create-only with conflict-fail
semantics. TXT is written first and JSON **last**; both are read back and source
ETag is checked before returning completion. An interrupted TXT-only write is
recoverable on retry without retranscribing or replacing the TXT. Modified,
unrelated, or inconsistent sidecars are conflicts, not permission to overwrite
user files. A valid empty Speech result is completion; malformed provider output
is never converted into silence.

Success is HTTP 200 with `v`, `status` (`completed` or `already_completed`),
`operation_id`, `json_item_id`, and `text_item_id`, never transcript text.
The operation ID is 64 hexadecimal characters and returned sidecar IDs are
bounded to 128 characters for the device response buffers.
Status never invokes Speech or downloads the WAV; it reconciles bounded remote
sidecars after restart and returns `not_started`, `processing`,
`already_completed`, or `retry_required`. Errors use
`{"v":1,"error":{"code":"...","retryable":false}}` and the stable codes in the
shared contract. HTTP 202 and fire-and-forget processing are not used. Warm
`/readyz` first, allow roughly 210 seconds at the client, and query status after
uncertain delivery rather than reuploading audio.

### Server-only voice tools

When enabled, the verified Voice Live session advertises only:

* `search_onedrive`: bounded filename search beneath the configured root.
* `get_onedrive_item`: item metadata with source name and timestamps.
* `read_onedrive_text`: bounded UTF-8 TXT/JSON/Markdown/WebVTT source content.

There is no device `tools.execute` message, generic remote execution, arbitrary
URL fetcher, write tool, or live-weather capability. Schemas reject extra
properties. Retrieval is bounded to ten results/page, 50 metadata items and
eight tool calls per user turn, 256 KiB per text fetch, 12 KiB output per call,
and 32 KiB total tool output per turn. Search is deliberately a bounded filename
scan, not an exhaustive full-text index; responses identify this limitation.

Function arguments accumulate by call/item ID, with an 8 KiB cap. Added/delta/
done and response-completion variants are reconciled; completed calls execute
once. Tool work runs separately from the provider reader, sends server-side
`function_call_output`, and requests one continuation only after all calls in
that response have completed. Speech interruption, new turns, stop, and session
expiry cancel stale work. Original repeated/interleaved audio deltas still
share their ordered response epoch and retain played-position truncation.

The system prompt requires source filenames/timestamps, distinguishes inference
from evidence, and labels transcript/metadata content as untrusted data rather
than instructions. These are explicit grounding/injection boundaries, not a
claim that prompt injection can be eliminated by a prompt alone.

### Validation and remaining live gates

The existing test runner includes real-source HTTP-mocked OBO, Graph, Speech,
processing/recovery, authenticated API, and voice-tool lifecycle tests alongside
the original voice regressions. No additional runtime dependency or lockfile
version change is needed: HTTPx and async Azure Identity were already pinned.

The operator reports a successful live MI prerequisite probe against the existing
resource's custom cognitive-services endpoint: one second of synthetic PCM16
mono 16 kHz silence, `{"locales":[]}`, HTTP 200 in approximately 0.3 seconds,
with an explicit 1000 ms duration and empty phrase arrays. This proves that
probe's authentication, input format, and valid no-speech behavior, not spoken
accuracy or this application's complete processing path.

Live acceptance remains an operator gate: verify real multilingual spoken audio,
measure a representative
30-minute input within the ingress deadline, verify consumer OneDrive sidecar
create/read/retry, and exercise real Voice Live function event/configuration
acceptance before setting `ONEDRIVE_TOOLS_ENABLED=true`. Mocked tests do not
establish those service-side capabilities or production latency. This code does
not create credentials/apps, request user sign-in, deploy resources, or claim
that these new end-to-end live gates have passed.

A fuller live `VoiceSession` diagnostic streamed paced, locally synthesized
16 kHz microphone PCM while consuming returned audio at real-time speed.
After the initial output-queue correction, a ten-frame simulated speaker with
100 ms played-progress batches completed 40,800 output samples without errors.
That cloud run preceded physical-board tuning. The final one-second hard credit,
500 ms prefill, and nominal 60 ms catch-up pass paced-speaker/session regressions
and the shared four-period DMA simulator without interior gaps.
The original 33-record output queue reproduced a normal-playback
`playback_backlog` failure; a regression now exercises an immediately produced
two-second response against a paced speaker. The output age bound was later
raised to three seconds after a real multi-turn `playback_backlog` failure.
The synthetic principal and CLI credentials were diagnostic-only; no public
server, consumer account enrollment, or production authentication bypass was used.

Do not add `response.modalities: ["text", "audio"]` to response creation:
the live native model rejected that explicit override. The production request
intentionally sends only `type: response.create` and inherits the accepted
session configuration; its audio response also includes transcript events.

The parent subsequently reproduced normal paced playback and a synthetic
barge-in through the real `VoiceSession`: one playback clear, two output epochs,
and no reported error. A manual private Container Apps job also ran the built
image using its production managed identity and accepted the exact Voice Live
configuration. See `..\docs\cloud-setup.md` for the resource/execution record.

Consumer enrollment and GitHub OIDC deployment have completed, and public
ingress reached `/readyz` from zero-capable hosting. **Authenticated physical
device conversation, cold-start WebSocket behavior, board acoustics/AEC/DMA
interruption, and BYOM availability still require acceptance.** The native
managed model is the deployed path; no BYOM fallback is configured.
