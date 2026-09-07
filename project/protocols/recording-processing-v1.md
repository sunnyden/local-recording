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
entry. `completed` clears it. Errors never erase the WAV or its successful
OneDrive-upload state. Retrying with the same source identity is deliberate.
