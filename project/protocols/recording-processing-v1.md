# Recording processing HTTP v1

This adds HTTP operations; `recorder.voice.v1` is unchanged.
All processing requests use an API-B bearer token. Microsoft Graph access
tokens and refresh tokens are never sent as API-B authentication.

## Process a finalized upload

`POST /v1/recordings/process`

JSON request, at most 4096 UTF-8 bytes:

```json
{
  "v": 1,
  "drive_id": "<Graph drive ID>",
  "item_id": "<Graph item ID>",
  "source_sha1": "<40 lowercase hex characters>",
  "source_size": 123456,
  "recorded_at": "2026-09-07T22:05:36+08:00"
}
```

`recorded_at` is optional. IDs, source size/hash and timestamp are untrusted
hints. The backend derives the owner from the validated API-B token and checks
the current Graph item, root membership, actual WAV format, bytes and duration.
No arbitrary URL, output path, model, language override or user ID is accepted.

The server derives a stable operation key from owner/drive/item/source hash
and processing-version/options. A duplicate request uses the same operation.
It must never require reuploading the WAV.

Success is HTTP 200 with bounded JSON:

```json
{
  "v": 1,
  "status": "completed",
  "operation_id": "<opaque identifier>",
  "json_item_id": "<Graph sidecar ID>",
  "text_item_id": "<Graph sidecar ID>"
}
```

`status` may also be `already_completed`. Both sidecars must be confirmed
before returning either status. The response never contains transcription text
or tokens. Filenames are `<wav stem>.transcription.json` and `<wav stem>.txt`.

## Query an uncertain result

`POST /v1/recordings/status` accepts the same bounded source identity request.
It returns HTTP 200 with `v`, `operation_id`, and `status`:
`not_started`, `processing`, `completed`, `already_completed`, or `retry_required`.
Completed responses include sidecar IDs. Status calls do not invoke Speech.
The server reconciles completed sidecars after a process restart.

## Errors

All errors use JSON `{"v":1,"error":{"code":"<stable code>","retryable":false}}`.
Do not expose raw provider messages. Expected codes/statuses:

| HTTP | Code | Meaning |
| --- | --- | --- |
| 400 | invalid_request | Malformed schema or unsupported version |
| 401 | authentication_required | Invalid/missing API-B authorization |
| 403 | consent_required | Graph delegated consent needs renewal |
| 403 | item_not_allowed | Item or user is outside configured scope |
| 404 | source_not_found | Source no longer exists |
| 409 | source_changed | Source fingerprint no longer matches |
| 409 | processing_in_progress | Retry status; do not start duplicate work |
| 409 | output_conflict | Sidecar conflicts with unrelated/manual content |
| 413 | recording_too_long | Above 30-minute or bounded size limit; WAV upload remains valid |
| 415 | unsupported_audio | Not supported PCM16 mono 16 kHz WAV |
| 429 | busy | Concurrency/rate limit; honor Retry-After |
| 503 | temporarily_unavailable | Transient backend/provider failure |
| 504 | processing_deadline | Synchronous operation deadline reached |

Warmup calls `/readyz` before POST when needed, with a bounded overall warmup
budget of 120 seconds. Processing uses a roughly 180-second backend budget and
210-second device HTTP timeout below the 240-second ingress limit.
After uncertain delivery, query status and retain the nonsecret device outbox
entry. `completed` marks it complete. Errors never erase the WAV or its successful
OneDrive-upload state. Retrying with the same source identity is deliberate.

## Progress-stream transport

`WSS /v1/recordings/process-stream`, subprotocol `recorder.processing.v1`,
is an additive processing-only transport. It is not the voice socket and
does not change `recorder.voice.v1`. Authenticate the upgrade using the
same API-B bearer in the Authorization header, never in the URL.

The client sends exactly one text message containing the process request
above, at most 4096 UTF-8 bytes. The server sends bounded text messages:

```json
{"v":1,"type":"started","operation_id":"<opaque identifier>"}
{"v":1,"type":"progress","phase":"downloading","completed_bytes":32768,"total_bytes":123456}
{"v":1,"type":"progress","phase":"transcribing"}
{"v":1,"type":"heartbeat","phase":"transcribing"}
{"v":1,"type":"result","operation_id":"<opaque identifier>","status":"completed","json_item_id":"<id>","text_item_id":"<id>"}
```

Allowed phases are `resolving`, `downloading`, `validating`, `transcribing`,
`saving`, and `verifying`. Byte counts are optional and only describe
measurable file transfer, not Speech completion. Fast Transcription does not
expose incremental percentage progress. Heartbeats, at most 10 seconds apart,
mean the proxy connection is alive, not that Speech has advanced.

A terminal failure is
`{"v":1,"type":"error","error":{"code":"<stable code>","retryable":false}}`,
using the same error codes as HTTP. A completed result requires both
confirmed sidecars. The client can send `{"v":1,"type":"cancel"}`.
Cancellation/disconnect cleans up the request-owned operation; it does not
detach a worker or enqueue a job. Existing HTTP/status clients remain supported.

The stream does not use the HTTP processing operation's 180-second total
deadline or the device's 210-second HTTP limit. Authentication expiry,
explicit cancellation, lost connection and upstream safety timeouts still
apply. In particular, no-progress detection must not confuse an opaque
Speech request with a stalled audio download. A connected stream is allowed
to continue while genuine download/write progress is being made.

The device warms the service as before and gives status requests 45 seconds,
exceeding the backend's 30-second status budget. It obtains a fresh API-B
access token before opening a processing stream and expects a heartbeat or
progress/result within 30 seconds once connected. On uncertain delivery it
reconciles via status rather than sending a second processing start blindly.
The SD receipt is retained until completion or confirmed remote-deletion
policy applies. The display says processing/checking/pending, not that a
successful WAV upload failed.
