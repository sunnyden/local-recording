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
tokens only on the device; never send refresh tokens to this service.

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
`tools: []` and
`tool_choice: none` are explicit; incoming function calls are fatal, not
executed. The exact model and required audio/VAD/AEC/tool settings must be
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
