# Multi-display freeze in existing virtual display drivers

_Status 2026-10-26: source review and issue-tracker evidence done; **measurements pending** (the elevated run of
`tools/vdd-stress/Run-VddRepro.ps1` on the dev PC). The "Results" section is filled in from that run._

The brief (§2, F1) says current VDDs freeze the system when more than one display is added, and asks for the root
cause before a fix is designed. This document covers the three drivers named there: SudoVDA, MikeTheTech's Virtual
Display Driver (MTT VDD) and parsec-vdd.

## What was examined

| Driver | Version | Source | How it creates displays |
|---|---|---|---|
| SudoVDA | 1.10.9.289 (shipped in Apollo 0.4.6) | SudoMaker/SudoVDA @ a4b09fa (derived from the Microsoft IddCx sample) | One IOCTL per display, at runtime; a 3 s watchdog removes all displays unless pinged |
| MTT VDD | 11.30.4.434 (driver-only package of release 25.7.23) | VirtualDrivers/Virtual-Display-Driver @ d724496 (master, already contains PR #482) | Fixed count from `vdd_settings.xml`, read when the adapter starts |
| parsec-vdd | 0.45.0.0 (WHQL) | Closed source; control API from nomi-san/parsec-vdd `core/parsec-vdd.h` | One IOCTL per display, at runtime; all displays are removed unless updated every < 100 ms |

Issue trackers read: VirtualDrivers/Virtual-Display-Driver (#36, #200, #246, #254, #319, #354, #449, #482, #490),
nomi-san/parsec-vdd (#53, #58), ClassicOldSong/Apollo (#179, #809, #1252, #1469).

## Findings, by the brief's hypotheses

### 1. Swap-chain threads that block or spin: **confirmed for MTT VDD (root cause of its multi-display collapse)**

MTT VDD keeps a single swap-chain worker per adapter, inherited from the single-monitor ge9/IddSampleDriver fork,
while it creates several monitors on that adapter. Every `EvtIddCxMonitorAssignSwapChain` replaces "the" worker:
the previous monitor's worker is destroyed, its swap chain is deleted when the worker exits, Windows assigns that
monitor a new swap chain, and that assignment destroys the next monitor's worker. With two or more displays
this is a loop, and only the most recently assigned display stays stable.

- The maintainers' fix, PR #482 ("Fix multi-monitor swapchain threading for issue #449", merged 2026-05), moves to one
  worker per monitor and describes exactly this mechanism. It also made the frame loop event-driven when no frame
  is pending, and stopped per-frame file logging across the workers.
- Reports that match it: #449 (performance collapse with 3+ virtual screens, also with 2), #354 (dozens of "Direct3D
  device created" log lines per second and 3 GB of log per day, i.e. swap chains re-created continuously), #490
  (4 monitors on build 26200: resets and bugchecks with DWM, DxgKrnl and IddCx activity right before them).
- **The latest MTT release (25.7.23) still ships driver 11.30.4.434, which predates the fix.** That is the build
  reproduced here.

SudoVDA follows the Microsoft sample's model of one context and one worker per monitor, so it does not have this bug. Its
worker waits for the next frame with a 16 ms timeout and then polls again, which is about 62 wake-ups per second per idle
display. That is wasteful but not a freeze. The worker's teardown (`WaitForSingleObject(thread, INFINITE)` inside
`EvtIddCxMonitorUnassignSwapChain`) is safe only because the worker never blocks without also waiting on the
terminate event. That property has to be kept by design.

### 2. Missing MMCSS registration: **not the cause**

Both open-source drivers register their workers with MMCSS: SudoVDA as "DisplayPostProcessing", MTT as "Distribution".
SudoVDA also raises the whole WUDFHost process to `HIGH_PRIORITY_CLASS`, which affects every UMDF driver that shares
that host process.

### 3. Swap chains on the wrong adapter: **a performance problem, not a freeze**

IddCx picks the render adapter unless the driver calls `IddCxAdapterSetRenderAdapter`. On a hybrid PC like the dev PC
(AMD iGPU and RTX 4060 Ti), frames can be rendered on one GPU and encoded on the other. SudoVDA exposes a
set-render-adapter IOCTL, which applies to the next swap chain. MTT matches a GPU by friendly name from its XML. MTT
#200 (apps report "GDI Generic") and Apollo #179 point at adapter and clock-domain mismatches that cause stutter rather
than hangs.

### 4. Oversized mode lists: **contributes; fails outright at the extreme**

- SudoVDA reports 47 fixed target modes plus 10 derived from the preferred mode (about 57 per display), plus the
  modes parsed from its EDID.
- MTT reports every XML resolution × every global refresh rate. The default is 5 × 6 plus 5, and users go much
  higher. In #254, 108 combinations make the monitor fail to start (status 0xC00000BB, `STATUS_NOT_SUPPORTED`).
- Windows intersects source, target and monitor modes for every path on every topology change, for all displays, not
  just the new one. Large lists multiply the per-change work by the display count.

### 5. Device re-enumeration storms: **confirmed by design in MTT and parsec-vdd; inherent in Windows**

- **MTT** can change its display count only by re-initialising the adapter. Every change removes and re-adds *all*
  displays. Its named-pipe "reload" path also has two problems:
  - It passes the pipe `HANDLE` to `WdfObjectGet_IndirectDeviceContextWrapper` (undefined behaviour), then calls
    `IddCxAdapterInitAsync` again on a live adapter.
  - The pipe is created with SDDL `D:(A;;GA;;;WD)`, so any local user can reconfigure or reload the driver (#490
    asks about this too).
- **parsec-vdd**: per its maintainer (#58), adding a display blocks the caller's message loop "a second and longer
  with more present displays". Because every display is removed unless the driver is pinged every 100 ms, one slow
  add can cascade into the loss of all displays. **SudoVDA** has the same all-or-nothing behaviour with a 3 s watchdog.
- **Windows itself**: each monitor arrival or departure triggers a topology re-evaluation, a `WM_DISPLAYCHANGE`
  broadcast to every top-level window, and DWM re-creating its swap chains for the outputs. This cost grows with the
  number of displays and running apps, and no driver can remove it. It can only be paid once instead of N times, by
  adding all displays first and applying the topology once.

### 6. HDR negotiation: **unverified; measured by the HDR on/off runs**

SudoVDA advertises `IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE | WIDE_COLOR_SPACE` and FP16 processing on every target,
whether or not the display is meant to be HDR. Windows then evaluates advanced colour per display when it arrives.
Whether this adds a mode set per arrival is to be measured.

### 7. Other defects found on the way

- **Cursor event leak:** both open-source drivers create a *named* event for hardware-cursor notifications on every
  swap-chain assignment. SudoVDA uses `"arbitraryMouseEventName"`, shared by all monitors and never closed; MTT uses
  `"VirtualDisplayDriverMouse"`, closed only on failure. Neither driver ever waits on the event or reads cursor data.
  This leaks one handle per re-assignment, which the MTT thrash makes frequent.
- **Periodic stutter with a physical DisplayPort monitor (Apollo #179):** on NVIDIA, with a virtual display plus a DP
  monitor, the stream stutters every 10 s exactly, and the stutter stops when the monitor is unplugged. That matters
  for the Phase 1 acceptance test "with physical monitors". The "hold" run (4 displays for 120 s) looks for periodic
  stalls.
- **Sleep/wake:** Apollo #1469 reports resume hangs while a SudoVDA client pings during wake. The maintainer disputes
  the diagnosis. Power transitions are not in this analysis; Tessera's driver handles D0 entry and exit explicitly.

## Working theory

"The freeze" is not one bug. Three mechanisms compound as displays are added:

1. **Driver bugs that turn one topology change into a loop.** The MTT single-worker thrash is the clear case. Fixable
   in the driver.
2. **Driver designs that make each change expensive or fragile:**
   - a D3D device created per swap-chain assignment, inside the IddCx callback (SudoVDA);
   - large mode lists (all three drivers);
   - whole-adapter restarts to change the count (MTT);
   - all-or-nothing keepalive watchdogs (SudoVDA, parsec-vdd).

   All fixable in the driver.
3. **Windows' own per-arrival cost**, which grows with the display count. Not fixable in the driver; it is mitigated
   by batching arrivals and having the host apply the topology once.

The measurements below say how much each mechanism contributes on real hardware.

## Reproduction method

- **`tools/vdd-stress/vdd-stress.exe`** adds and removes displays through each driver's own control interface (no
  vendor app), with 1, 2, 4 and 8 displays per cycle. It times each change from the request until the display is
  active or gone in the CCD topology, and runs four probes meanwhile:
  - `compose`: a 4×4 topmost window is redrawn and `DwmFlush` waits for each frame. A gap means the screen froze.
  - `ui`: `WM_NULL` round trips to Explorer's taskbar. A gap means the shell's UI thread was blocked.
  - `sched`: a time-critical thread sleeps 1 ms. A gap means the scheduler was starved.
  - CPU of dwm.exe, WUDFHost.exe and explorer.exe, once per second.
- **`tools/vdd-stress/Run-VddRepro.ps1`** (one elevated run, unattended, resumable after a crash) runs a 60 s
  baseline. Then for each driver it does a stress run (10 cycles per count) and a hold run (4 displays for 120 s).
  Each run is recorded with WPR: GPU.light, CPU.light, plus IddCx, Display, DisplaySwitch, Kernel-PnP and UMDF
  events. The relevant System and Application log entries are exported, and the driver, its trust entries and
  settings are removed afterwards.
- Pass/fail is judged against the brief's targets (§4.5): each change takes under 3 s, no stall over 250 ms, no hang.
- Baseline on the dev PC without any VDD (2026-10-26, Windows 11 IoT Enterprise LTSC 26100): compose max 18 ms (one
  60 Hz frame), ui max 0.4 ms, sched max 2.9 ms.

**Caveat:** the dev PC also restarts on its own (see STATUS). A restart during a run is recorded as "crashed" with the
Kernel-Power and BugCheck events. It is not attributed to the driver unless it repeats.

## Results

_Pending the elevated run._

## Consequences for Tessera's driver

See `docs/phase-1-design.md`. In short:

- one worker per monitor, event-driven, with no polling and no I/O on the frame path;
- one D3D device per render adapter, shared, created outside the IddCx callbacks;
- at most 8 modes per display;
- runtime add/remove without adapter restarts;
- displays tied to the controlling handle, with no all-or-nothing timer;
- an admin-only control interface;
- an explicit render adapter;
- HDR advertised only on displays that ask for it;
- no hardware-cursor setup until the cursor is actually consumed.
