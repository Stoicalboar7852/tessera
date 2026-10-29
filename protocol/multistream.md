# multistream v1: one video stream per client monitor

Constants and structures: `protocol/include/tessera/protocol.h`. Decision record: ADR 0007.

Stream 0 is the session's ordinary GameStream video stream, unchanged. Streams 1 .. k-1 are added by this extension.
A client or host that doesn't support it never sees any of the following.

## 1. Capability (`/serverinfo`)

The host adds two elements to the `root` of its `/serverinfo` reply:

| Element | Value |
|---|---|
| `TesseraMultiStream` | highest supported version (1) |
| `TesseraMaxStreams` | streams it can run now, stream 0 included: the minimum of its configuration, `TESSERA_MAX_STREAMS` and the encoder sessions its GPU allows |

A client uses multi-stream only if `TesseraMultiStream` ≥ 1, `TesseraMaxStreams` ≥ 2 and it wants more than one
display.

## 2. Display list (`/launch`, `/resume`)

The client adds `tmsv=1&tmsd=<list>` to its `/launch` (or `/resume`) request.

`<list>` is one entry per client monitor, joined by `~`. Each entry has 9 fields, joined by `.`:

| # | Field | Rules |
|---|---|---|
| 1 | monitor key | lowercase hex, 2–64 digits, even length. Stable per client monitor (the client's own monitor ID, hashed if longer). Unique within the list. |
| 2 | width | pixels, 640–8192 |
| 3 | height | pixels, 480–8192 |
| 4 | refresh numerator | refresh = numerator / denominator Hz, 23–500 Hz (60000/1001 for 59.94) |
| 5 | refresh denominator | ≥ 1 |
| 6 | x | position in the client's layout, in the same pixel units as width and height; `-` allowed; \|x\| ≤ 32767 |
| 7 | y | as x |
| 8 | DPI | 48–1200, or 0 if unknown |
| 9 | flags | decimal; bit 0 = HDR |

Entry 0 is the client's primary monitor; its width, height and refresh must match `mode=`. The list holds at most
16 entries and 2048 characters, and the host takes the first k it can run. Example (two monitors, the second to the
right and HDR):

```
tmsv=1&tmsd=a1b2c3d4.2560.1440.60.1.0.0.109.0~e5f60718.1920.1080.60000.1001.2560.180.92.1
```

The host validates the whole list before it acts. If the list is invalid, the client lacks the permission, or the
host can't create displays, the session runs with one stream (today's behaviour) and the reply says why.

**Reply:** the launch/resume XML adds `TesseraStreams` (k, the streams this session will have, ≥ 1) and, when k is
less than the list's length, `TesseraReason` (a token: `host-limit`, `encoder-limit`, `not-permitted`,
`no-display-driver`, `display-failed`, `invalid-request`, `unsupported`).

**Host behaviour:**
- One virtual display per accepted entry, added in one request.
- The monitor ID is the first 16 bytes of SHA-256(`"tessera-monitor-v1"` ‖ client certificate PEM ‖ key bytes).
- The physical size is derived from the DPI, and the name is `<client name>-<n>`.
- The displays are arranged like the client's layout, to the right of the host's existing desktop, with entry 0 as
  the primary. If the client's rectangles overlap or don't touch, they are placed left to right by x instead, top
  edges aligned.

## 3. RTSP

- **DESCRIBE:** the host adds `a=x-tessera.streams:<k>`.
- **SETUP:** for each extra stream i (1 .. k-1), the client sends `SETUP streamid=video/<i>/0`. The reply is as for
  video, with that stream's own `X-SS-Ping-Payload` (16 random characters).
- **ANNOUNCE:** the client sends `x-nv-video[i].*` and `x-nv-vqos[i].*` for every stream, with the same attributes as
  stream 0.
  - **Per stream:** `clientViewportWd`, `clientViewportHt`, `maxFPS`, `clientRefreshRateX100`,
    `videoEncoderSlicesPerFrame`, `maxNumReferenceFrames`, `dynamicRangeMode`, `bw.maximumBitrateKbps`,
    `fec.minRequiredFecPackets`, plus `x-ml-video[i].configuredBitrateKbps` (the stream's share before the FEC
    allowance, as `x-ml-video.configuredBitrateKbps` is for stream 0).
  - The client also sends `x-tessera.streams:<k>` with the count it will run (≤ the DESCRIBE value).
  - **Format:** the negotiated codec for every stream, 10-bit only for streams whose display is HDR
    (`dynamicRangeMode`).
  - **Taken from stream 0:** codec (`bitStreamFormat`), `packetSize`, `encoderCscMode`, chroma sampling, QoS type.
  - **Bitrate:** the client splits its total bitrate by pixel rate (width × height × fps), with at least
    1000 kbit/s per stream. The host caps the sum at its configured maximum, scaling every stream alike.

## 4. Video

- **Transport:** every stream is sent from the host's video port. The client opens one UDP socket per stream and pings
  the host's video port from it with that stream's payload; the host sends the stream to the address and port the ping
  came from.
- **Packets:** packetization, RTP and FEC are as for stream 0, and each stream has its own sequence numbers and frame
  indices.
- **Encryption:** every stream of a multistream session is encrypted (`SS_ENC_VIDEO` format), stream 0 included, so
  all streams share stream 0's packet size (which leaves room for the encryption header). The host offers
  `SS_ENC_VIDEO` in DESCRIBE for such sessions whatever its encryption mode, and the client turns it on. The IV is
  the stream's 64-bit packet counter (bytes 0–7, little-endian), then `00 00 <i> 'V'`.

## 5. Control messages (ENet control stream, encrypted like the rest)

| Type | Direction | Payload |
|---|---|---|
| `0x5400` IDR request | client → host | `tessera_ctrl_stream_t` |
| `0x5401` invalidate reference frames | client → host | `tessera_ctrl_invalidate_t` |
| `0x5410` HDR mode | host → client | `tessera_ctrl_stream_t`, then the legacy HDR mode payload (enabled byte + `SS_HDR_METADATA`) |
| `0x5411` stream status | host → client | `tessera_ctrl_stream_status_t` |

The legacy messages (IDR, reference-frame invalidation, HDR mode, loss statistics) keep meaning stream 0. Messages
naming stream 0 or a stream ≥ k are ignored. A stream that stops (encoder or capture failure) is reported with its
reason; the other streams continue. Extra streams have no LTR acknowledgements or loss statistics in v1.

Reasons 10 (`decoder-failed`) and 11 (`no-video`) are client-side only: common-c reports them to its application
when an extra stream's decoder can't start or no complete frame arrives.

## 6. Input

`TESSERA_MOUSE_POSITION_MAGIC` (`0x54000001`) carries an absolute position on one stream's display
(`tessera_mouse_position_t`). The host maps it onto that display's rectangle. The legacy absolute-mouse packet keeps
meaning stream 0. Keyboard, relative mouse, scroll, gamepads, touch and pen stay as they are.
