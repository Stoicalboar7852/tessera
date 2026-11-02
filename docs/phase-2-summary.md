# Phase 2 summary: protocol and host multi-stream

_Completed and confirmed 2026-11-02. Details are in `docs/STATUS.md`, `docs/perf.md`,
`docs/phase-2-design.md`, `protocol/multistream.md` and ADR 0007._

## Acceptance

Brief §8: "a test client receives 2–4 streams, and stock Moonlight still works", detailed in
`docs/phase-2-design.md`. Results are from acceptance run 4 (2026-11-02, `logs\mstream\20261102-1639`, 5-minute
cases) unless noted. Everything ran on the dev PC over localhost.

| Criterion | Result | Evidence |
|---|---|---|
| 1. A test client receives 2, 3 and 4 streams for 5 minutes each, at 1080p60 and as a mixed set; every stream decodes and shows its own display; no frames lost | ✅, with a Windows finding | 2, 3, 4 streams and 2560×1440@60 + 1920×1080@59.94: zero FEC failures, one IDR per stream and none requested later, each snapshot shows its own display. The host delivers every frame Windows composes (counted per display). With video content all four streams run at 59.7–59.8 fps; with GDI windows repainting at about 64 Hz Windows composes 51–58 fps per display when several are busy (finding 1). The test client checks the bitstreams with FFmpeg snapshots; it doesn't decode every frame. |
| 2. Stock Moonlight pairs, streams for 2 minutes and quits; the host behaves as upstream | ✅ | Moonlight 6.2.0 portable: paired, streamed Desktop, quit; the host log shows a normal session with no multistream plan. |
| 3. The test client against stock Sunshine falls back to one stream and works | ✅ | Stock Sunshine (latest release): one stream, 11,210 frames in 5 minutes. |
| 4. Extra latency per additional display ≤ 2 ms | ✅ on localhost; LAN not measured | Host processing latency 1.7 / 1.9 ms (p50 / p95) with one stream, stock Sunshine 1.6 / 1.8 ms. Each additional display adds at most 1.1 ms p50 and 1.7 ms p95 to the slowest stream with every display busy, and nothing measurable with one busy display (the design's aim: ≤ 1 ms typical). |
| 5. Displays added in one request, arranged as the client's monitors, removed at quit with the topology restored exactly; the same after a host kill | ✅ | `topoctl compare` matched after the multistream cases, after the kill and at the end. The kill case: both virtual displays were gone when checked 8 s after the host died (the driver's grace period is 2 s), and `recover.exe` restored the journaled topology. The mouse test lands at the centre of each stream's display. |
| 6. Too many streams are answered with fewer and a reason; encoder session limits probed and reported | ✅ | Host limited to 2, client asks for 4: 2 streams, `host-limit`. The encoder probe reports 4 of 4 sessions on the RTX 4060 Ti. |
| 7. Unit tests for the pure parts pass in CI | ✅ | host-helpers (display list, planning, layout, monitor IDs, bitrate split) and common-c (Linux, macOS, Windows, including the symbol check of the per-stream copies). |

**Not verified:**
- A LAN client (needs a second PC, design decision 5), Wi-Fi and remote links.
- A client that decodes and displays every stream: the Qt client is Phase 3.
- HDR streams, and mixed HDR and SDR displays.
- Re-claiming displays within the driver's grace period (a host service restart).
- AMD and Intel encoders, and their session limits.
- Display changes during a session (the client's monitors changing).

## What exists now

- **Protocol** multistream v1 (`protocol/multistream.md`, `protocol/include/tessera/protocol.h`, ADR 0007):
  - `/serverinfo` capability (`TesseraMultiStream`, `TesseraMaxStreams`);
  - a display list in `/launch`, answered with the stream count and a reason;
  - per-stream RTSP SETUP and ANNOUNCE attributes;
  - extra streams on the existing video port, every stream encrypted with its own IV space;
  - `0x54xx` control messages (IDR, RFI, HDR mode, stream status) and per-stream absolute mouse input.
- **common-c**: up to 4 streams. Each extra stream is a compiled copy of the unchanged upstream video path; about 80
  marked lines in upstream files.
- **Host**:
  - one capture thread and one encoder per display, sharing one control, input and audio channel;
  - virtual displays per client monitor, added in one request, laid out like the client's monitors with its primary
    as Windows' primary, journaled and restored;
  - the encoding GPU set as the driver's render adapter;
  - the concurrent-encoder probe;
  - options `tessera_max_streams`, `tessera_display_source` and `tessera_make_primary`, and a per-client
    "Multi-display" switch in the web UI;
  - per-display capture statistics in the debug log.
- **Tools**:
  - `tools/mstream-client`: pairs, launches with a display list, records per-stream statistics and raw video;
  - `Run-MultiStreamTest.ps1`: the acceptance matrix, elevated and unattended, about 50 minutes;
  - `Prepare-MultiStreamTest.ps1`: stages the CI host build and the stock releases.

## Findings worth knowing

1. **Windows composes fewer frames per virtual display when several show content that updates slightly faster than
   their refresh rate.** With GDI windows repainting at about 64 Hz on 60 Hz virtual displays, Windows composed
   about 60 fps with one busy display, 55–58 with two, 51–54 with three and 48 with four (run 3). Desktop
   Duplication delivered every composed frame, so this happens below the host. Content that presents at the display
   rate (video, Direct3D, the animated wallpaper) got 60 fps on all four displays, and at 120 Hz every content frame
   arrived. Whether the driver can influence it is not known. A possible mitigation is to create the virtual
   displays at twice the stream's frame rate, as Apollo offers, at the cost of more composition work and apps
   rendering faster.
2. **Latency per display comes from sharing the one encoder engine.** It was lowest with video content (+0.3 ms
   p50 per display) and highest with four independent GDI timers (+1.1 ms). Raising the bitrate to 20 Mbps per
   display changed nothing on localhost.
3. **Stock Moonlight is only portable when started in its own folder** (`portable.dat` is looked up in the working
   directory). Run 2 left its identity and paired host in the user's registry.
4. **A launch whose RTSP handshake fails stays pending on the host until its ping timeout** (upstream behaviour), so
   a failed attempt can affect the next client for a few seconds.
5. **The RTSP request buffer had to grow** from 2 to 8 KiB for the ANNOUNCE of four streams.

## Next: Phase 3 (Qt client multi-monitor)

Monitor discovery, layout mirroring, one window per monitor and edge-crossing mouse. *Acceptance:* a 2-monitor
Windows client and a 2-monitor macOS client each stream 2 host displays. A design for Phase 3 comes first, for
approval.
