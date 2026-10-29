# ADR 0007: Multi-stream protocol extension

- **Status:** Proposed (2026-10-30, with `docs/phase-2-design.md`)

## Context

F2 needs one session with one video stream per client monitor, sharing one control channel, one input channel and
one audio stream (brief §6). §4.3 requires full compatibility with stock Moonlight clients and stock Sunshine hosts,
negotiation through `/serverinfo` and the RTSP exchange, and encryption of every new channel with the session keys.

The GameStream protocol as Sunshine and moonlight-common-c implement it has:
- one UDP video socket on the host (port 47998) for all sessions. The host finds a session's address from the client's
  ping, which carries a per-session payload returned by RTSP SETUP (`X-SS-Ping-Payload`);
- per-frame packetization with RTP and an NV video header, Reed-Solomon FEC, and optional AES-GCM per packet with a
  deterministic IV (64-bit counter, fixed byte `'V'`);
- an SDP-like ANNOUNCE whose video attributes are already indexed (`x-nv-video[0].*`, `x-nv-vqos[0].*`);
- an ENet control channel with 16-bit message types. Sunshine's own extensions use `0x55xx`, and its input magics
  `0x550000xx`.

## Decision

**Stream 0 is the existing stream, unchanged. Streams 1..n-1 are negotiated additions.**

1. **Capability:** `/serverinfo` adds `<TesseraMultiStream>1</TesseraMultiStream>` (the extension version) and
   `<TesseraMaxStreams>`. The client uses multi-stream only if both are present and it wants more than one display.
2. **Display list in `/launch` and `/resume`**, as `tmsv=1&tmsd=<entries>`. Each entry has the client monitor key
   (hex, at most 32 bytes), width, height, refresh numerator and denominator, its x/y in the client's layout, DPI and
   HDR. Entry 0 matches `mode=`. The host replies in the launch XML with `<TesseraStreams>` and, when it accepts fewer
   streams, `<TesseraReason>`. It goes in `/launch` because the displays must exist before the app starts.
3. **RTSP:** DESCRIBE adds `a=x-tessera.streams:<k>`. For each extra stream the client sends
   `SETUP streamid=video/<i>/0`, and the host returns a separate random 16-byte ping payload. ANNOUNCE carries
   `x-nv-video[i].*` and `x-nv-vqos[i].*` for i < k.
4. **Transport:** all streams use the host's existing video port. The client uses one UDP socket per stream and pings
   with that stream's payload. Each stream has its own RTP sequence numbers, frame indices, FEC blocks and receive
   buffer.
5. **Encryption:** extra streams are always encrypted (AES-GCM, the session's video key). The IV is a 64-bit
   per-stream counter followed by the fixed field `00 00 <i> 'V'`. That is the NIST SP 800-38D deterministic
   construction, and for i = 0 it is exactly today's IV. IVs are therefore unique under the one key across all streams.
6. **Control messages** in a new range `0x5400`–`0x54FF`, each with a stream index:
   - client → host: IDR request, reference-frame invalidation, FEC/loss status;
   - host → client: HDR mode, stream status (running, stopped with a reason code).
   
   Legacy messages keep meaning stream 0.
7. **Input:** a new magic `0x54000001` carries an absolute mouse position with a stream index and reference size.
   Everything else in the input channel is session-wide, as today.
8. **Constants and structures** are defined once in `protocol/include/tessera/protocol.h` (C), used by the host and
   common-c. The full spec is `protocol/multistream.md`, registered in `protocol/extensions.md` as `multistream` v1.

## Alternatives considered

- **One socket for all streams, demultiplexed by RTP SSRC.** It is standard RTP, but it is one kernel receive buffer
  and one receive thread on the client for every stream: a burst on one stream drops packets on another. It would
  also need changes in the depacketizer's packet path. Separate sockets reuse the existing per-stream path and the
  existing ping mechanism.
- **A new UDP port per stream.** It needs new firewall and port-forwarding rules, for no gain over per-stream pings
  on the existing port.
- **Display list in RTSP ANNOUNCE.** Too late: the app is launched at `/launch`, before RTSP, and must open on the
  new displays.
- **A separate key per stream (HKDF).** Equally safe, but a new key-derivation step on both sides; a distinct fixed
  field per stream is enough for IV uniqueness and matches how Sunshine already separates its channels.

## Consequences

- Stock clients and stock hosts see no difference. A Tessera client against a stock host gets one stream, and a stock
  client against a Tessera host gets today's session.
- Each of the four client platforms implements the display list and the per-stream windows, using common-c's shared
  launch-query builder and API.
- Parsers for the display list, the per-stream ANNOUNCE attributes and the `0x54xx` messages are untrusted input.
  They get bounds checks now and fuzz targets in Phase 12.
- Changing the meaning of a field later needs `multistream` v2 (both sides use the highest common version).
