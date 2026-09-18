# Recorder voice protocol v2

Version 2 extends, and does not replace, `recorder.voice.v1`. A device selects
WebSocket subprotocol `recorder.voice.v2` on `/v1/voice`. Authentication,
transport limits, live PCM frames, playback accounting, cancellation, and
control semantics are unchanged except that every control has `"v":2`.

## Rolling context handshake

The first message is the v1 `hello` shape with `"v":2` and one additional,
required field:

```json
{"v":2,"type":"hello","sample_rate":16000,"channels":1,"format":"pcm16","frame_samples":320,"context":{"format":"ogg_opus","length":0,"sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}}
```

`length` is the encoded byte length from 0 through 262144. `sha256` is exactly
64 lowercase hexadecimal characters and covers those encoded bytes. The
context is Ogg Opus, mono, with a 16000 Hz original-input-rate field. It must
decode to nonempty signed PCM16 mono at exactly 16000 Hz and at most 30 seconds
(960000 bytes). An empty context is represented by length zero and the empty
SHA-256 digest.

For nonempty context, binary context messages immediately follow `hello`.
Each is at most 4096 bytes and has this 20-byte little-endian header:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `ERC2` |
| 4 | 1 | Version, exactly 2 |
| 5 | 1 | Kind, exactly 1 (context) |
| 6 | 2 | Reserved, zero |
| 8 | 4 | Sequence, starting at zero |
| 12 | 4 | Encoded-byte offset, starting at zero |
| 16 | 4 | Payload byte length |

Payloads are nonempty and at most 4076 bytes. Sequence increments by one and
offset increments by the preceding payload length. Chunks must total the
declared length exactly. Wrong length, order, offset, digest, container, codec,
channel count, original rate, decoded size, or any text/control before the
declared context is complete aborts the session with a visible protocol error.
Encoded or decoded audio remains in memory and is never persisted or logged.

The proxy opens the provider only after complete validation and decoding. It
configures semantic VAD with `create_response:false`. For nonempty context it
creates one server-ID-assigned user message with one `input_audio` content part
and waits for the matching item-created confirmation. This bypasses VAD
segmentation. It
never sends `response.create` for context and sends device `ready` only after
the context item is confirmed. Empty context skips item creation. Any nonempty context
failure aborts visibly; microphone/live flow cannot begin before `ready`.
