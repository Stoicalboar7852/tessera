# Phase 2 design: protocol and host multi-stream

_Status: **proposal, waiting for approval** (2026-10-30). Nothing below is implemented. The wire protocol is in
ADR 0007 (proposed with this design)._

## Goal and acceptance (brief §8, F2, §4.3, §4.5)

The goal is one session that carries one video stream per client monitor. The host creates a virtual display for
each client monitor, arranges the displays like the client's monitors, and runs one capture → encode → RTP pipeline
for each. All streams share one control channel, one input channel and one audio stream. The client's bandwidth is
split across the streams by pixel rate, and frame pacing and FEC stay per stream.

**Acceptance (brief):** a test client receives 2–4 streams, and stock Moonlight still works. In detail:

1. **Multi-stream:** a scripted test client receives 2, 3 and 4 streams for 5 minutes each, at 1080p60 and with a
   mixed set (2560×1440 + 1920×1080). Every stream decodes, each shows its own display (a per-display snapshot), and
   on the same PC no frames are lost.
2. **Stock clients:** the latest stock Moonlight release pairs, streams for 2 minutes and quits, and the host behaves
   as upstream does (one stream, the configured display).
3. **Stock hosts:** the test client against the latest stock Sunshine release falls back to one stream and works.
4. **Latency (§4.5):** extra latency per additional display, measured per stream for 1, 2, 3 and 4 streams at
   1080p60, goes in `docs/perf.md`. The target is ≤ 2 ms per additional display.
5. **Display lifecycle:** displays are added in one request and arranged as the client's monitors. They are removed
   when the app quits, and the host topology is then restored exactly (`topoctl compare`). The same holds after the
   host is killed mid-session (watchdog).
6. **Limits:** a request for more streams than the host allows is answered with fewer streams and a clear reason.
   Encoder session limits are probed and reported (§4.5).
7. Unit tests for the pure parts pass in CI.

## What changes, at a glance

```
client (test client now; Qt in Phase 3)          host (Sunshine fork)
┌────────────────────────────────────┐ HTTPS ┌──────────────────────────────────────────────┐
│ /serverinfo: TesseraMultiStream?   │──────▶│ nvhttp: advertise; /launch reads the display │
│ /launch ...&tmsv=1&tmsd=<displays> │       │ list → tessera::display_session (VDD batch   │
│                                    │       │ add, layout, primary, journal)               │
│ common-c: RTSP DESCRIBE/SETUP      │ RTSP  │ rtsp: per-stream SETUP (ping payload) and    │
│   video/0..n-1, ANNOUNCE           │──────▶│ ANNOUNCE x-nv-video[i] / x-nv-vqos[i]        │
│   x-nv-video[i] per stream         │       │                                              │
│ stream 0: upstream video path      │  UDP  │ stream: one capture→encode thread per stream,│
│ streams 1..3: same code, compiled  │◀──────│ one broadcast socket (47998), per-stream     │
│   as separate instances            │ 47998 │ RTP sequence, FEC, AES-GCM IV space          │
│ control: 0x54xx per-stream msgs    │ ENet  │ control: per-stream IDR/RFI/loss/HDR/status  │
│ input: per-stream absolute mouse   │──────▶│ input: per-stream touch ports                │
└────────────────────────────────────┘       └──────────────────────────────────────────────┘
```

**Stream 0 is exactly today's stream.** A stock client never sees anything new, and a Tessera client against a stock
host never asks for anything new. Extra streams (1..n-1) are additions next to it.

## Protocol (summary; full rationale in ADR 0007)

- **Capability:** `/serverinfo` adds `<TesseraMultiStream>1</TesseraMultiStream>` (extension version) and
  `<TesseraMaxStreams>n</TesseraMaxStreams>`. Stock clients ignore unknown elements.
- **Display list:** `/launch` and `/resume` take `tmsv=1&tmsd=<list>`. Each entry holds the client monitor's key (hex,
  at most 32 bytes, stable per client monitor), width, height, refresh as numerator/denominator, its position in the
  client's layout, DPI and HDR. Entry 0 is the client's primary and must match `mode=`. The host answers in the launch
  XML with `<TesseraStreams>k</TesseraStreams>` and, if k < requested, `<TesseraReason>`. Displays must exist before
  the app starts, which is why this goes in `/launch` (the same place Sunshine configures displays today) and not in
  RTSP.
- **RTSP:**
  - DESCRIBE adds `a=x-tessera.streams:k`;
  - the client sends `SETUP streamid=video/<i>/0` for each extra stream and gets its own `X-SS-Ping-Payload`;
  - ANNOUNCE carries `x-nv-video[i].*` and `x-nv-vqos[i].*` for every stream. GameStream's SDP is already indexed, so
    no new syntax is needed.
- **Video transport:**
  - Every stream goes to the existing video port. The client pings from one UDP socket per stream with that stream's
    payload, and the host learns each stream's address from it. This needs no new ports or firewall rules.
  - Each stream has its own RTP sequence, frame index and FEC.
- **Encryption:** extra streams are always AES-GCM encrypted with the session's video key. The IV is NIST SP 800-38D
  deterministic: a 64-bit per-stream counter, then the fixed field `00 00 i 'V'`. Stream 0 keeps its existing
  negotiation and IV, which is the same construction with `i = 0`.
- **Control messages** (new range `0x54xx`; Sunshine uses `0x55xx`):
  - client → host: IDR request, reference-frame invalidation and FEC/loss status for stream i;
  - host → client: HDR mode for stream i, and stream status (for example "stopped: encoder limit").
  
  The legacy messages keep meaning stream 0.
- **Input:** a new magic `0x54000001` carries an absolute mouse position with a stream index. Keyboard, relative mouse,
  gamepads and audio stay session-wide.
- **Shared constants** live in a new C header, `protocol/include/tessera/protocol.h`, used by both the host and
  common-c. The host keeps its upstream moonlight-common-c submodule unchanged. The spec is written to
  `protocol/multistream.md` and registered in `protocol/extensions.md`.

## Host (`host/`, C++20)

New code goes in `host/src/tessera/`. Upstream files get small hooks, each marked `// [fork] Tessera`.

| New module | Does |
|---|---|
| `multistream.{h,cpp}` | Parses and validates the display list and the per-stream ANNOUNCE attributes; builds the stream plan; derives monitor IDs. Pure functions, unit-tested. |
| `display_session.{h,cpp}` | Opens the VDD with `vdd_client`, sets the render adapter to the encoding GPU, and adds all displays in one request (ADR 0006). It waits for them, then applies the client's layout to the right of the existing desktop and makes stream 0 the primary, in one journaled topology change (`journaled_display_change`). At revert it removes the displays in one request and restores the topology. |
| `encoder_limits.{h,cpp}` | At startup, after Sunshine's encoder probe, opens up to 4 concurrent 1080p encoder sessions on the chosen GPU to find how many work. Reported in `/serverinfo` and the log. |
| `client_permissions.{h,cpp}` | Per-paired-client permissions (§4.3), stored in `tessera_clients.json` next to Sunshine's state, so the upstream state format stays as is. "Multi-display streaming" defaults to on. |

**Monitor IDs** are the first 16 bytes of SHA-256("tessera-monitor-v1" ‖ client certificate ‖ client monitor key).
They are stable per client monitor, so Windows keeps position and scaling for it (ADR 0006). Friendly names are
`<ClientName>-<n>`, as F4 will use.

**Hooks in upstream files:**
- **`nvhttp.cpp`:** the `serverinfo` elements; in `launch`/`resume`, parse `tmsd` and check the permission and limits.
  It calls `display_session` instead of `display_device::configure_display` for multi-display sessions, and adds the
  launch XML elements. Reverting follows Sunshine's existing call sites (app quit, cancel).
- **`rtsp.{h,cpp}`:** `launch_session_t` gets a `tessera` member (plan and per-stream ping payloads); DESCRIBE adds the
  stream count; SETUP handles `video/<i>`; ANNOUNCE parses the per-stream attributes.
- **`stream.cpp`:**
  - `session_t` gets per-stream video state for streams 1..n-1 (peer, sequence, cipher, IV counter, mailbox);
  - one video thread per stream, and a per-stream ping wait;
  - the broadcast thread picks the stream's state by the packet's stream index;
  - handlers for the `0x54xx` messages.
- **`video.{h,cpp}`:**
  - `capture()` takes the display to capture;
  - the shared async capture context becomes one per display name (today it is one global);
  - packets carry a stream index.
  
  Encoders without parallel encoding (the sync path) stay single-stream, and the host says so.
- **`input.cpp`:** one touch port per stream, and the new absolute-mouse magic.
- **`config.cpp`:**
  - `tessera_max_streams` (default 4);
  - `tessera_display_source = virtual | physical`, where "physical" maps streams to the active physical displays left
    to right and needs no VDD;
  - `tessera_make_primary` (default on).
- **`confighttp.cpp` and the web UI:** the permission toggle in the client list.

**Unchanged for stock clients:** no `tmsd` means today's path, including Sunshine's own display configuration.

## common-c (`/common-c`, C11)

common-c is a single-connection library: its video path keeps about 30 file-scope globals (`VideoStream.c`,
`VideoDepacketizer.c`). There are two ways to get several streams:

- **Proposed: compile the upstream video path once per extra stream.**
  - A generated wrapper, `TesseraVideoInstance<i>.c`, defines a few macros and `#include`s `VideoStream.c` (and
    another wrapper does `VideoDepacketizer.c`). Their `static` state is then separate per instance.
  - `TesseraVideoInstance.h` renames the instance's entry points (`initializeVideoStream` → `...Instance1`, …) and
    binds its few shared references (`StreamConfig`, `VideoCallbacks`, `VideoPingPayload`, the frame-loss callbacks)
    to stream i's slot.
  - The upstream files stay byte-identical. A CI check lists the undefined symbols of the instance objects and fails
    on any not in an allowlist, so an upstream change that adds a new shared reference can't silently bind to stream
    0.
  - Three instances give 4 streams in total.
- **Alternative: refactor the globals into a per-stream struct.** It is cleaner to read, but it changes hundreds of
  lines in upstream files and makes every upstream merge of the video path a manual job. It remains the fallback if
  the instance approach meets something it can't handle.

**New files:**
- `Tessera.h`: the public API;
  - `LiTesseraSetDisplays()` before `LiStartConnection()`, with per-stream decoder callbacks and contexts;
  - `LiTesseraGetStreamCount()`, `LiTesseraGetStreamInfo()` and `LiTesseraSendMousePosition(stream, ...)`;
  - a launch-query builder that all clients share.
- `TesseraStreams.c`: the stream slots, start/stop of the extra streams, and the bandwidth split.
- `TesseraControl.c`: per-stream control messages.
- Unit tests in `common-c/tests/`, run by `ci-common-c` on Linux, macOS and Windows:
  - the bitrate split;
  - the launch-query format;
  - the IV construction, using test vectors shared with the host's tests.

**Small upstream edits:**
- `RtspConnection.c`: per-stream SETUP and reading `x-tessera.streams`;
- `SdpGenerator.c`: the per-stream `x-nv-video[i]` blocks;
- `ControlStream.c`: one exported send function, plus dispatch of `0x54xx` messages to `TesseraControl.c`;
- `Connection.c`: start and stop the extra streams.

**Bandwidth split:** each stream gets `total × pᵢ / Σp`, where `p = width × height × fps`, with a 1 Mbps floor and
the rest renormalised. This is computed in common-c so every client splits the same way. The host caps the sum at its
`max_bitrate`.

The clients' own copies of moonlight-common-c (Qt, Android and iOS submodules) switch to `/common-c` in the phase that
touches each client (Qt in Phase 3). Phase 2 doesn't change any client app.

## Driver (`drivers/vdd`), small

The ADD descriptor gains the monitor's physical size, derived from the client's DPI, and the EDID carries it. Windows
then recommends a scale close to the client monitor's. This is interface v3; v2 requests are still accepted.

## Test client and test harness

- **`tools/mstream-client`** (C++20, built with MinGW like the other native tools, plus MSVC in CI). A command-line
  client on `/common-c`:
  - `pair` (the PIN protocol, with OpenSSL);
  - `stream --displays 1920x1080@60,2560x1440@60 --seconds N`, and `--legacy` for one stream;
  - it decodes each stream with FFmpeg (MSYS2 package), writes a snapshot per stream, and reports per-stream frames,
    losses, FEC recoveries, host processing latency, and receive-to-complete time as JSON.
- **`tools/tests/Run-MultiStreamTest.ps1`** (elevated, unattended, like `Run-VddAcceptance.ps1`):
  - starts a test instance of the host with its own config folder and generated test web credentials (kept only in the
    run folder);
  - pairs the test client and stock Moonlight through the PIN API;
  - places a labelled window on each display;
  - runs acceptance items 1–6 and restores everything;
  - writes `results.md`.
  
  Stock Moonlight and stock Sunshine are downloaded portable releases, run one at a time on this PC over localhost.

## Order of work

1. ADR 0007, `protocol.h`, `protocol/multistream.md`; the pure host parsers and the common-c pure functions, with
   tests.
2. common-c: the instance build with its symbol check, SETUP/ANNOUNCE, control and input messages.
3. Host: the per-display capture context, per-stream video state and broadcast, RTSP and nvhttp hooks.
4. Host: `display_session` (VDD, layout, primary, journal), the encoder probe, permissions and config.
5. Driver v3 (physical size in the EDID).
6. The test client and the harness; acceptance runs; `docs/perf.md` latency rows; the Phase 2 summary.

## Risks

| Risk | Mitigation |
|---|---|
| Compiling common-c's video path several times is unusual, and an upstream change can break it | Upstream files stay unchanged; the CI symbol check catches new shared references; the refactor is the fallback. |
| Hooks in `stream.cpp` and `video.cpp` (large upstream files) grow and make merges hard | New logic lives in `host/src/tessera/`; hooks pass a stream index or a display name. The diff to upstream files is measured and kept small. |
| One broadcast thread sends every stream's frames in turn, so later streams wait. About 0.35 ms per 1080p frame, but several ms at 4K and high bitrates | Measured in acceptance item 4. If it misses ≤ 2 ms at 1080p60, or stalls at 4K, give each stream its own sender thread with a shared pacing budget. |
| Capturing several IddCx displays with Desktop Duplication, and GPU copies between adapters | The render adapter is set to the encoding GPU (this exercises the `SET_RENDER_ADAPTER` IOCTL, unverified in Phase 1); WGC remains Sunshine's alternative. |
| Encoder session limits (NVIDIA consumer cards limit concurrent NVENC sessions) | Probed at startup and advertised; a stream that still fails is reported to the client by a status message, and the others continue. Never silent. |
| Display changes freeze the desktop (ADR 0006) | One batched add, then one layout change. About 2 s at session start, inside the baseline. Nothing is chained. |
| Localhost tests don't show LAN behaviour | The latency rows say "localhost". A LAN run needs a second PC (question below). |
| The host must open the admin-only VDD interface | The installed service already runs the host as SYSTEM. Test runs need an elevated harness, started by you. |

## Not in Phase 2

- Client UI and windows per monitor, edge-crossing mouse, per-stream pen and touch (Phase 3).
- Turning off the physical monitors (Phase 4).
- Several clients, input policies and display naming in menus (Phase 5).
- Android and iOS (Phase 11).
- Virtual displays for stock clients: today's behaviour stays (decision 2 below).

## Decisions needed from you

1. Approve this design (or say what to change).
2. **Stock clients keep today's behaviour** (they stream the configured display) and virtual displays are only for
   Tessera multi-display sessions. The alternative is that every session gets a virtual display at the client's
   resolution, as Apollo does; that could be added later as an option.
3. **The client's primary monitor becomes Windows' primary display** during a multi-display session (journaled and
   restored afterwards), so apps open where the client looks. The alternative is that the host's primary stays as it
   is.
4. **common-c approach:** compile the video path once per extra stream (proposed), or refactor it.
5. **Is a second PC or laptop available** on your LAN for one latency run? Otherwise the latency numbers are
   localhost-only for now.
