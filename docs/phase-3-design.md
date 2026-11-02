# Phase 3 design: Qt client multi-monitor

_Status: **approved 2026-11-02** as written, with decisions 1–7 as recommended. Builds on Phase 2
(`docs/phase-2-design.md`, ADR 0007, `protocol/multistream.md`); the build change is ADR 0008._

## Goal and acceptance (brief §8, F2)

The Qt client streams several host displays in one session, one borderless fullscreen window per client monitor. It
finds the client's monitors, sends them in the launch request, opens a window on each monitor it gets a stream for,
and sends the mouse to the stream under the cursor, so moving past a window's edge carries the cursor onto the
neighbouring display.

**Acceptance (brief):** a 2-monitor Windows client and a 2-monitor macOS client each stream 2 host displays. In detail:

1. **Windows client** (a laptop with one external monitor, over the LAN to the dev PC): 2 streams for 10 minutes.
   Each window shows its own host display, fullscreen on its own monitor. Every stream decodes in hardware with no
   decoder errors, and the per-stream statistics show the expected frame rate.
2. **macOS client** (the MacBook Pro with one external monitor): the same.
3. **Mouse:** in absolute mode ("optimize mouse for remote desktop") the cursor crosses from one window to the next
   and the host cursor follows onto the matching display, in both directions. In relative (game) mode the host
   cursor crosses displays by itself and the client keeps the mouse captured. Clicking into the other window does
   not drop keys or release capture.
4. **Choice of displays:** "this display", "all displays" and a chosen subset each start the right number of
   windows on the right monitors. A single-monitor client looks and behaves as upstream Moonlight.
5. **Fallback:** against stock Sunshine, or a host that gives fewer streams, the client streams what it gets and
   says why (host limit, encoder limit, not permitted for this client, …).
6. **Mixed rates:** a 120 Hz and a 180 Hz (or 60 Hz) monitor stream at their own rates in one session.
7. **No regressions:** a single-display session behaves as upstream (same windows, settings and statistics).
   Unit tests for the new pure code pass in CI.

## What changes, at a glance

```
Qt client                                                       host (unchanged from Phase 2)
┌────────────────────────────────────────────────────────────┐
│ Settings: Displays = this one / all / chosen               │
│ tessera::Monitors   → TESSERA_DISPLAY list (key, mode,     │
│                       position, DPI, HDR)                  │
│ NvHTTP::startApp    → /launch ...&tmsv=1&tmsd=<list>       │──▶ virtual displays laid out like the monitors
│ Session (stream 0)  → upstream window + decoder, unchanged │◀── stream 0
│ tessera::StreamWindow ×(n-1) → own SDL window, decoder,    │◀── streams 1..3
│                       pacer, renderer, on its own monitor  │
│ input: mouse events → stream of the window under cursor    │──▶ LiTesseraSendMousePosition(stream, …)
│ common-c: /common-c (multistream), not the Qt submodule    │
└────────────────────────────────────────────────────────────┘
```

**Stream 0 stays upstream's stream:** the existing `Session` window, decoder and event loop. Extra streams are new
objects next to it, and the event loop hands their events to them. With one display nothing new runs.

## Components

### 1. Build: the Qt client uses `/common-c` (ADR 0008, new)

`clients/qt/moonlight-common-c/moonlight-common-c.pro` points `COMMON_C_DIR` at the monorepo's `/common-c` (imported
from the same upstream commit, f900dd47, as the Qt client's submodule) and adds the `Tessera*.c` sources and
`/protocol/include`. The submodule stays in `.gitmodules` but is no longer built; removing it from all clients is a
later clean-up. `ci-client-qt.yml` also runs on changes to `common-c/**` and `protocol/**`.

### 2. common-c additions (small, unit-tested)

The Qt decoder needs per-stream versions of three calls it makes for stream 0 today:

```c
void LiTesseraRequestIdrFrame(int stream);                 // after a decoder reset
bool LiTesseraGetCurrentHostDisplayHdrMode(int stream);
bool LiTesseraGetHdrMetadata(int stream, PSS_HDR_METADATA metadata); // stored from TESSERA_CTRL_HDR_MODE
```

The HDR metadata is already in the `0x5410` message; common-c only has to keep it per stream. Stream 0 maps to the
upstream functions.

### 3. Monitor discovery: `app/streaming/tessera/monitors.{h,cpp}` (+ `_win.cpp`, `_mac.mm`)

One `ClientMonitor` per OS monitor: name, `TESSERA_DISPLAY` (key, size, exact refresh, position, DPI, HDR) and
whether it is the OS primary.

| Field | Windows | macOS |
|---|---|---|
| Key (stable) | Hash of the monitor's device path from `QueryDisplayConfig` (EDID-based, survives port changes on most systems) | Hash of `CGDisplayCreateUUIDFromDisplayID` |
| Size | Native pixel mode (`StreamUtils::getNativeDesktopMode`) | Native pixels below the notch (the same function's safe area), so the host display matches the visible area |
| Refresh | Exact fraction from `QueryDisplayConfig` (59.94 = 60000/1001) | `CGDisplayModeGetRefreshRate`, snapped to the nearest exact fraction |
| Position | `SDL_GetDisplayBounds` (pixels; the app is per-monitor DPI aware) | Points × each screen's backing scale, normalized so monitors touch (see below) |
| DPI | `GetDpiForMonitor` | 96 × backing scale factor |
| HDR | DXGI output colour space is HDR10 (`IDXGIOutput6::GetDesc1`) | `NSScreen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1` |

**Layout normalization** (pure function, unit-tested): macOS positions are in points and screens can have different
scales, so the client rebuilds the layout in pixels: monitors keep their left-to-right order and vertical alignment,
and each touches its neighbour. Windows positions are already pixels and pass through. The host only needs the
order and adjacency (Phase 2 lays the virtual displays out from them).

**Per-display mode:** each stream uses its monitor's native resolution and refresh rate (180 Hz stays 180 Hz). A
setting "Display resolution: Native / Desktop size" lets a user stream a Retina or 4K monitor at its scaled desktop
size to save encoder and network load; the client then upscales.

### 4. Choosing displays: `app/streaming/tessera/displayselection.{h,cpp}` + settings

New preferences, in the same settings store as upstream's but under their own keys (`tessera/displays`,
`tessera/chosenDisplays`, `tessera/displayResolution`, read and written by `tessera::DisplaySettings`, so
`StreamingPreferences` stays unchanged):
- displays: `This display` (default, upstream behaviour) / `All displays` / `Chosen displays`;
- chosen displays: the keys of the chosen monitors;
- display resolution: `Native` / `Desktop size`.

The settings page gets a "Displays" section (`gui/TesseraDisplays.qml`, included from `SettingsView.qml`, backed by
the QML singleton `TesseraDisplaySettings`) with the list of monitors (name, resolution, refresh, HDR) and
checkboxes. It is hidden when the client has one monitor.

**Stream 0** is the OS primary if it is chosen, else the monitor showing the Moonlight window. The host makes it the
Windows primary, so the taskbar and new windows land where the user expects.

**Bitrate:** with automatic bitrate on, the total is the sum of upstream's default bitrate for each display (capped
at the slider maximum); with a custom bitrate, the user's value is the total. common-c splits it by pixel rate.

**Launch:** `NvHTTP::startApp` appends `tmsv=1&tmsd=<list>` when the host advertises `TesseraMultiStream` and more
than one display is chosen, and returns `TesseraStreams` / `TesseraReason`. Fewer streams than asked for, or a stock
host, produce a message in stream 0's status overlay naming the reason, for example "multi-display is not enabled for
this client in the host's web UI" (the launch banner is gone by then). With automatic bitrate, the total then covers
only the displays that stream. Problems known before launch (fewer than two chosen displays connected, more than four
chosen) use the launch banner.

### 5. Stream windows: `app/streaming/tessera/multidisplay.{h,cpp}` and `videostats.{h,cpp}`

`MultiDisplay` (owned by `Session`) holds the plan and one `StreamWindow` record per extra stream, and passes
`LiTesseraConfigure()` the displays, the extra decoder callbacks (with the `StreamWindow` as renderer context) and
the `TESSERA_CALLBACKS`.

Each extra stream has:
- its own SDL window, created on its monitor and made borderless fullscreen (`SDL_WINDOW_FULLSCREEN_DESKTOP`);
- its own `FFmpegVideoDecoder` (and so its own renderer, pacer and V-sync source, which already follow the window's
  monitor), chosen with the same `Session::chooseDecoder()` and the same preferences. It pulls its frames, so no
  submit trampolines are needed, and only pull renderers can stream several displays;
- a decoder lock, and the window-event handling that recreates the decoder on size, display or device changes,
  following the upstream code for stream 0.

The decoder learns its stream index (`DECODER_PARAMETERS::tesseraStream`) and uses the per-stream pull, IDR, HDR and
statistics calls (stream 0 maps to upstream's). About twenty marked lines in `ffmpeg.cpp`.

**Event routing:** before the event loop's `switch`, `MultiDisplay::handleEvent()` takes window events for extra
windows (by window ID). Frame-ready events from main-thread renderers' pacers carry no window, so on each one every
extra decoder renders too (a renderer with no new frame does nothing); `pacer.cpp` stays unchanged.

**Messages after launch** (fewer streams than chosen, a stopped stream) go to stream 0's status overlay for 10
seconds, because the launch banner is only shown before the stream starts.

**HDR:** each window switches HDR when the host says so for its stream (`TESSERA_CALLBACKS.setHdrMode`). Stream 0
keeps the upstream callback.

**A stream that stops** (host reports `TESSERA_STREAM_STOPPED`): its window shows a short message and closes; the
other streams continue.

**A streamed monitor that disappears** (unplugged, or the laptop lid closes): in v1 the session ends with a clear
message. Changing streams mid-session needs a protocol extension and comes later.

**Statistics overlay:** the overlay stays on the stream 0 window (upstream's overlay manager has one renderer) and
gains one line per extra stream (resolution, fps received/decoded/rendered, decode and render time, drops). Every 10
seconds the client also writes each stream's statistics to its log, which the tests read.

**Fullscreen toggle (Ctrl+Alt+Shift+X)** applies to all windows. Windowed multi-display puts each window on its own
monitor at 80 % size; it is mainly for testing on one machine.

**macOS:** multi-display sessions use `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=0` (borderless windows on the current
Spaces, no Space-switch animation, and the cursor moves freely between monitors). The menu bar and Dock are hidden
while the app is active.

### 6. Input: `input/mouse.cpp`, `input/input.cpp` (marked edits)

- **Absolute mode:** a mouse event's window ID selects the stream. The position inside that window's video area goes
  to `LiTesseraSendMousePosition(stream, x, y, w, h)`. The pointer is never locked to a window in a multi-display
  session, so the OS cursor crosses monitors normally and the host cursor follows onto the matching display. Moving
  onto a client monitor that is not streamed leaves the video, as moving out of a window does today.
- **Relative mode:** unchanged. The focused window holds relative capture and the host moves its cursor across its
  displays, which are laid out like the client's monitors, so the host cursor appears in the right window.
- **Focus:** focus moving between our own windows is not a focus loss. On `FOCUS_LOST`, the handler checks whether
  another stream window now has keyboard focus; if so it does not raise keys, mute audio or release capture.
  Keyboard grab ("capture system keys") follows the focused stream window.
- Buttons, wheel, keyboard, gamepads and touch are session-wide and unchanged.

## High refresh rates and VRR (requested 2026-11-02)

**High refresh (e.g. a 180 Hz monitor and a 120 Hz laptop):** this needs no doubling. Each stream runs at its
monitor's own rate: the virtual display is created at that rate (the driver accepts 23–500 Hz) and the stream asks
for that frame rate. Games present through DXGI and get every frame onto the virtual display; Phase 2's shortfall
only happened with GDI windows repainting slightly faster than the refresh rate on several busy 60 Hz displays. What
is not known yet, and Phase 3 measures:
- **Encoder throughput.** 2560×1440 at 180 Hz plus a MacBook Pro panel at 120 Hz is about 1.3–1.6 gigapixels per
  second on the RTX 4060 Ti's single NVENC engine, more than the Phase 2 tests asked of it. The "Desktop size"
  option above is the fallback if it can't keep up.
- **180 Hz virtual displays** have not run yet. At 2560×1440 the timing is too fast for an EDID detailed timing, so
  the driver describes the 60 Hz variant in the EDID and the exact mode in its mode list; this needs a real run.
- **Client decoding** of two high-rate streams on the MacBook Pro and on the Windows laptop.

**Doubling the virtual display's refresh rate (Apollo's option):** not recommended. At 180 Hz it would mean a 360 Hz
virtual display: the desktop and windowed apps would compose and render twice as often for no visible gain, and the
encoder would see more frames to skip. If the Phase 3 measurements show missing frames at high rates, adding it as a
host option is small.

**VRR** has two halves:
- **Client display:** show each frame as soon as it is decoded and let the monitor's VRR (G-Sync/FreeSync, or
  ProMotion on the MacBook Pro) adapt. Upstream Moonlight does this when V-Sync is off; Phase 3 checks that it works
  per window on each monitor. This part fits in Phase 3.
- **Host display:** a virtual display has a fixed refresh rate, so a game at, say, 97 fps is composed on a 180 Hz
  grid and each frame waits up to 5.6 ms (8.3 ms at 120 Hz) for the next refresh before it is streamed. A virtual
  display that does VRR itself would remove that wait. I don't know yet whether Windows' indirect display model
  (IddCx) allows it; that needs research. I'd record it as a feature request and look at it after Phase 3.

## Files to touch

New (Tessera only): `clients/qt/app/streaming/tessera/` (`monitors*`, `displayselection*`, `multidisplay*`,
`videostats*`, `inputrouting.cpp`, `displayplan.{h,cpp}` for the layout and stream order), `clients/qt/app/gui/TesseraDisplays.qml`,
`clients/qt/tests/tessera/` (QtTest for layout and selection), `docs/adr/0008-clients-use-common-c.md`,
`docs/tests/phase-3-acceptance.md`, `tools/tests/Run-QtClientTest.ps1`, `tools/tests/Collect-ClientLogs.ps1` / `.sh`.

Changed, marked `[fork] Tessera` (upstream files): `moonlight-common-c.pro`, `app/app.pro`, `app/main.cpp` (register
the settings singleton), `app/qml.qrc`, `streaming/session.{h,cpp}` (configure, create and route extra windows, focus
check, teardown), `backend/nvhttp.{h,cpp}` (launch arguments and reply), `video/decoder.h` and
`video/ffmpeg.{h,cpp}` (stream index), `input/{input.h,input.cpp,mouse.cpp}`, `gui/SettingsView.qml` (include the
section).
Estimate: 250–350 marked lines, most in `session.cpp` and `mouse.cpp`.

Also: `common-c/src/Tessera.h`, `TesseraControl.c`, `TesseraStreams.c` and tests; `.github/workflows/ci-client-qt.yml`;
`docs/STATUS.md`, the platform matrix and `docs/perf.md`.

## Tests

- **CI:** common-c unit tests for the new calls; a QtTest target (layout normalization, display selection, stream 0
  choice, bitrate totals) built and run by `ci-client-qt.yml` on Windows and macOS; the Windows and macOS builds.
- **Dev PC, unattended** (`Run-QtClientTest.ps1`, one elevated run like Phase 2's): the CI host and the local Qt
  client build on the same PC. The client's extra monitors are virtual displays held by `vdd-stress` ("stand-ins"),
  the client streams all displays, windowed, with `tessera_make_primary` off (so Windows doesn't move the client's
  windows). Cases: one display, 2–4 streams at 1080p60, mixed sizes, 120, 144 and 180 Hz, borderless fullscreen, a
  stand-in unplugged mid-session, multi-display not permitted, host-limit, stock Sunshine. Checks: the streams the
  host accepted, each stream's received and rendered fps against its content and refresh rate, network and pacer
  drops, where each window is, the client quitting cleanly, and the host's capture statistics. The mouse checks use
  absolute mode and move the cursor themselves: on one PC the host moves the same cursor, so the harness looks for
  the cursor landing at the matching point of the stream's host display. Relative mode, focus and keyboard need the
  client machines. A stream stopped by the host has no trigger on one PC and is left to the client machines' logs.
  Not elevated, the run does the one-display and stock Sunshine cases only (a smoke test).
- **Manual acceptance** on the Windows laptop (Wi-Fi) and the MacBook Pro (gigabit Ethernet), each with an external
  monitor, following `docs/tests/phase-3-acceptance.md`, with the host from `tools/tests/Start-LanTestHost.ps1`
  (elevated: driver, firewall rules for the local subnet, pairing prompt, clean-up) and the logs from
  `tools/tests/Collect-ClientLogs.ps1` / `.sh`. The Mac on Ethernet also gives the wired LAN latency numbers Phase 2
  couldn't measure (design decision 5); the laptop gives Wi-Fi numbers.

## Build and iteration

The Qt client builds in CI today (about 9 minutes on Windows and 7 on macOS). For faster iteration I'll try a local Windows build
(Qt 6 from aqtinstall into `T:\toolchain`, MSVC from the EWDK, upstream's `setup-deps.ps1`). If the EWDK can't link
it, CI stays the only build. macOS builds only in CI; I can't run anything on the Mac myself.

## Risks

1. **Several hardware decoders and renderers in one process** (two to four D3D11VA or VideoToolbox sessions). Weak
   laptop GPUs may refuse a second hardware decoder. Mitigation: each window probes its decoder; if one falls back
   to software decoding, the launch warning says so.
2. **Focus and keyboard grab across windows** (Alt+Tab, the Windows key, macOS shortcuts) is where upstream's
   single-window assumptions run deepest. Covered by the manual tests.
3. **macOS fullscreen without Spaces** with the notch, the menu bar on several screens and ProMotion pacing needs
   the real Mac; I can only check it through the user.
4. **Upstream drift:** `session.cpp` and `mouse.cpp` are busy upstream files. Keeping extra streams in new files and
   only adding hooks keeps the merge surface small.
5. **High refresh rates** (encoder throughput, 180 Hz virtual displays, client decoding) are unmeasured; see above.
6. **Same-PC tests can't cover input or real fullscreen**, so mouse crossing and focus rely on the two client
   machines.

## Out of scope for Phase 3

Android and iOS clients (later phases); adding or removing displays during a session; per-window statistics
overlays; VRR on the virtual display; doubling the virtual display refresh rate.

## Decisions for approval

1. Build the Qt client against `/common-c` (ADR 0008). **Recommended.**
2. Extra streams as new `StreamWindow` objects next to the unchanged stream 0 path, rather than reworking `Session`
   into N symmetric streams. **Recommended** (smaller upstream diff; stream 0 keeps upstream's behaviour exactly).
3. Statistics overlay on the stream 0 window only, with a line per extra stream. **Recommended.**
4. A streamed monitor disappearing ends the session in v1. **Recommended.**
5. Each stream at its monitor's native resolution and refresh rate, with a "Desktop size" option. **Recommended.**
6. No refresh-rate doubling; measure first. **Recommended.**
7. VRR: client-side presentation checked in Phase 3; virtual-display VRR recorded as a feature request for after
   Phase 3. **Recommended.**
