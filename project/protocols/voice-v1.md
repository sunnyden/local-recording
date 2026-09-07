# Recorder voice protocol v1

The device connects to `wss://<proxy>/v1/voice` with
`Authorization: Bearer <API-B-access-token>` and WebSocket subprotocol
`recorder.voice.v1`. Tokens must never appear in a URL. The proxy authenticates
the user and client before opening a billable provider connection.

## Audio

Both directions carry signed little-endian PCM16, 16000 Hz, one channel.
Nominal packets contain 320 samples (20 ms). A packet may contain 1-320 samples.
Audio is not WAV or base64 on this connection.

Every binary WebSocket message starts with this 24-byte little-endian header:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `ERV1` |
| 4 | 1 | Version, exactly 1 |
| 5 | 1 | Kind: 1 microphone, 2 playback |
| 6 | 2 | Reserved, zero |
| 8 | 4 | Unsigned stream epoch |
| 12 | 4 | Unsigned packet sequence |
| 16 | 8 | Unsigned first sample position in this stream |

The rest is PCM. Reject invalid magic/version/kind, reserved bits, odd/empty
payloads, payloads larger than 640 bytes, and inconsistent sequence/sample
positions. Sequence starts at zero and increments per packet. Sample position
starts at zero and increments by the packet's sample count. Reconnect starts
a new session; never resend microphone audio from an old session.

Microphone epoch is zero. Each assistant output stream has a nonzero epoch
allocated by the proxy. Frames for a cleared or obsolete output epoch must
never reach the speaker. WebSocket fragmentation is transport-level and must
be reassembled before parsing, within the same limits.

## Control messages

Text messages are UTF-8 JSON objects with `v: 1` and a string `type`.
Maximum encoded size is 4096 bytes. Unknown message types are explicit protocol
errors, not silently ignored. Optional future capabilities require negotiation.

| Direction | Type | Additional fields |
| --- | --- | --- |
| device -> proxy | `hello` | `sample_rate:16000`, `channels:1`, `format:"pcm16"`, `frame_samples:320` |
| proxy -> device | `ready` | `session_id`, `max_session_seconds` |
| proxy -> device | `state` | `state`: connecting, listening, speaking, or stopping |
| proxy -> device | `playback.start` | `epoch` |
| proxy -> device | `playback.clear` | `epoch` |
| device -> proxy | `playback.cleared` | `epoch`, `played_samples` |
| device -> proxy | `playback.progress` | `epoch`, `played_samples` |
| proxy -> device | `playback.end` | `epoch` |
| either | `stop` | optional `reason` |
| proxy -> device | `error` | stable `code`, redacted `message`, `retryable` boolean |

The device sends `hello` first. `ready` is emitted only after the upstream
provider has accepted its session configuration. Microphone streaming starts
after `ready` and continues during assistant playback. The proxy owns VAD,
echo cancellation configuration, response creation and interruption handling.

`playback.progress` reports samples actually consumed for playback, not received
or queued samples. `playback.clear` immediately flushes the matching playback
queue; the acknowledgment reports the final played position. The proxy uses
that position to reconcile the provider conversation on interruption. Late
frames for that epoch are discarded. A subsequent `playback.start` uses a new
epoch and restarts sequence/sample position at zero.

The v1 device hard budget is 16000 sent-but-unreported samples (one second).
The proxy initially prefills 8000 samples (500 ms), then paces PCM at 16000
samples/second with catch-up bursts no larger than 60 ms. The fixed 50-frame
software ring can absorb a coalesced TCP delivery; combined software/DMA
accounting still enforces the one-second hard bound.
Report progress at least every 100 ms and immediately after final playback
drain. A normal next epoch starts only after the previous epoch's end and final
played progress; an explicitly acknowledged clear is the interruption exception.

The firmware reports fully completed DMA blocks. Clearing mid-block can
conservatively underreport fewer than 320 samples (20 ms); it must never
overreport queued data as heard. The clear acknowledgment is valid only after
old software and DMA audio are invalidated. If TX clearing cannot preserve
capture safely, stop the session rather than send a false acknowledgment.

Bound queue capacity and queue age. Overflow or excessive delay cancels the
response/session with an explicit error; never grow a queue without limit.
Use WebSocket ping/pong for liveness. Both sides close resources on disconnect.

## Provider boundary and extension points

The initial provider is Azure Voice Live with `gpt-realtime-2`, raw mono input,
service-side VAD and server-reference echo cancellation. Provider JSON/event
names do not appear in the device protocol. Any output sample-rate conversion
happens in the proxy.

Future tool messages must be separately negotiated and schema-validated.
Version 1 does not execute remote tools. Provider failures must not switch
models, disable authentication, or downgrade to half-duplex implicitly.
