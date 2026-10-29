# Phase 1 design: Tessera virtual display driver

_Status: **approved 2026-10-27 and implemented**; results in `docs/phase-1-summary.md`. Based on
`docs/vdd-freeze-analysis.md`. The stall target was later split (ADR 0006)._

## Goal and acceptance (brief, Phase 1)

The goal is a driver that adds and removes N monitors at runtime over an IOCTL interface, with per-monitor resolution,
refresh rate (including 59.94 and 119.88), HDR on/off and a stable ID. It sets an explicit render adapter, works with
zero physical monitors, and does not freeze.

**Acceptance:** `vdd-stress` adds and removes 8 displays 100 times, headless and with the physical monitors on. Every
add and remove completes in under 3 s, with no stall over 250 ms and no hang (§4.5). Results go in `docs/perf.md`.

## Components

```
host service / tools                     WUDFHost.exe (LocalService)
┌──────────────────────────┐   IOCTL    ┌──────────────────────────────────────────────┐
│ host-helpers/common      │──────────▶│ drivers/vdd  TesseraVdd.dll (UMDF 2 + IddCx)  │
│   vdd_client  (C++20)    │  admin-    │  control queue (sequential)                  │
│   edid / modes (pure)    │  only      │  adapter: render-adapter LUID, D3D device    │
└──────────────────────────┘  interface │           cache (one per LUID)               │
  used by: vdd-stress (tessera backend),│  monitor[i]: id, modes, EDID, worker thread  │
  vdd-install query, Phase 2 host       └──────────────────────────────────────────────┘
```

### Driver (`drivers/vdd`, C++17, UMDF 2.25+ / IddCx 1.4 baseline)

- **Object model.** Each monitor gets its own context (its WDF object context): ID, mode list, EDID, HDR flag,
  connector index and its swap-chain worker. Nothing is shared between monitors except the adapter-wide D3D device
  cache. This rules out MTT's single-worker thrash by construction.
- **Swap-chain worker** (one thread per monitor with an assigned swap chain):
  - Waits on `{new-frame event, terminate event}` with `INFINITE`. No timeouts, no polling.
  - Registers with MMCSS as "DisplayPostProcessing". The process priority stays as it is.
  - In Phase 1 the worker acquires a frame and releases it straight away; the host captures with DDA or WGC. Direct
    swap-chain capture into a shared texture is the optional F1 path, behind a flag, later.
  - No logging and no file or registry I/O on the frame path. TraceLogging events only on state changes.
  - `UnassignSwapChain` signals the worker and waits for it to exit. That is safe because the worker never blocks
    outside that one wait; the design rule is checked in code review. If the exit takes longer than 100 ms, a trace
    event is logged.
- **D3D device per render adapter.** The device is created once per adapter LUID and shared by all of that adapter's
  monitors. It is recreated when the device is removed or the DXGI factory is no longer current. Device creation
  happens lazily on the worker thread, not in the IddCx callback. An add burst therefore costs one device per GPU, not
  one per monitor per assignment.
- **Monitors on demand.** `IOCTL_TESSERA_VDD_ADD_MONITORS` takes 1 to 16 monitor descriptors per call and reports
  them with `IddCxMonitorArrival` back to back. `REMOVE_MONITORS` reports `IddCxMonitorDeparture`. The adapter is never
  restarted to change the count. Requests run on a sequential queue. No driver lock is held across IddCx calls that
  call back into the driver, because arrival synchronously triggers mode queries and swap-chain assignment.
- **Ownership instead of watchdog timers.** Monitors belong to the file handle that added them. When that handle
  closes (host stopped or crashed), the driver removes its monitors after a 2 s grace period. A host that reconnects
  within the grace period can re-claim them by ID, for example after a service restart. There is no periodic ping,
  and nothing removes all displays when one call is slow.
- **Modes.** Each monitor reports the requested mode first (preferred), plus at most 7 safe modes (1280×720, 1920×1080,
  2560×1440, 3840×2160 at 60 Hz; the requested resolution at 60 Hz). Target modes equal monitor modes, so there is no
  intersection blow-up. Fractional rates are exact rationals (60000/1001 for 59.94, 120000/1001 for 119.88), with
  CVT-RB timings.
- **EDID.** Generated per monitor:
  - The stable ID (128-bit, from the host per client monitor) sets the EDID serial and product code, and a name-based
    container ID (UUID v5). Windows can then remember position and DPI per client monitor (F1).
  - HDR monitors get a CTA-861 extension with HDR static metadata. Only they report the high and wide colour target
    caps, and only on IddCx ≥ 1.10 (Windows 11 22H2+), behind a version check. SDR monitors advertise nothing HDR.
- **Render adapter.** `IOCTL_TESSERA_VDD_SET_RENDER_ADAPTER` (LUID) is applied with `IddCxAdapterSetRenderAdapter`
  before the first monitor arrives. When it changes later, the affected swap chains are abandoned so that Windows
  re-assigns them on the new GPU. The host passes the encoding GPU's LUID (F1). With no LUID, IddCx's own choice is
  used and reported.
- **Hardware cursor.** Not set up in Phase 1, so Windows composes the cursor into the frames. That avoids the cursor
  event leak seen in SudoVDA and MTT. It can come later with a consumer, when the F1 direct-capture path needs it.
- **Security.** The device interface ACL in the INF allows only SYSTEM and Administrators (already true in Phase 0).
  Every request has a size and version header and is validated, and the input buffer is copied before use.
- **License.** Written from the IddCx documentation, not from the MS-PL sample or any of the drivers above, which were
  read only for this analysis. GPLv3 like the rest of the project. **ADR 0005** records this.

### Control interface v2 (`drivers/vdd/include/tessera_vdd_ioctl.h`, plain C, shared with MinGW)

| IOCTL | In | Out |
|---|---|---|
| `GET_VERSION` (exists) | — | interface version, driver version, max/active monitors, IddCx version, render-adapter LUID |
| `SET_RENDER_ADAPTER` | LUID | — |
| `ADD_MONITORS` | count, then per monitor: 16-byte ID, width, height, refresh as numerator/denominator, HDR flag, friendly name (UTF-16, 32 chars max) | per monitor: status, OS target ID, adapter LUID |
| `REMOVE_MONITORS` | count, IDs | per monitor: status |
| `LIST_MONITORS` | — | IDs, modes, owner (this handle / other), swap-chain state |

`TESSERA_VDD_INTERFACE_VERSION` goes to 2; Phase 0 `GET_VERSION` callers keep working (size-prefixed structs).

### Host side (`host-helpers/common`, C++20, MSVC and MinGW)

- `tessera/vdd_client.h`: opens the interface, provides add, remove, list and set-render-adapter, owns the handle (and
  so the monitors), and returns `result_t`. Phase 2's host uses it unchanged.
- `tessera/display_modes.h`: pure functions for CVT-RB timing, the mode list and EDID bytes. They are unit-tested in
  `host-helpers/tests` and compiled into the driver too, as header-only code with no STL exceptions.

### Tools and tests

- `vdd-stress` gets a `tessera` backend (through `vdd_client`) and a `--headless` option. With `--headless`, the
  physical paths are switched off through the state journal (`journaled_display_change`) once the first virtual
  display is active, and restored at the end. The watchdog and `recover.exe` cover a crash.
- `Run-VddAcceptance.ps1`: 8 displays × 100 cycles, headless and with physical monitors, plus HDR on/off and mode
  59.94. It writes `docs/perf.md` rows.
- In the Secure Boot test VM, `Test-VddInVm.ps1` is extended: install, add and remove 4 monitors, the
  handle-close-removes-monitors check, and uninstall.
- CI (`ci-driver`) builds and packages the driver, and the native CI builds `vdd-stress` and runs the mode/EDID unit
  tests.

## Order of work

1. ADR 0005 (driver license, written-from-docs rule); mode and EDID functions with unit tests.
2. Driver: per-monitor objects, the add/remove IOCTLs, the event-driven worker, the per-adapter device cache, and the
   render adapter.
3. `vdd_client` plus the `tessera` backend in vdd-stress; VM test.
4. Dev-PC acceptance runs (with physical monitors, then headless), `docs/perf.md`, the analysis results section, and
   the Phase 1 summary.

## Risks

| Risk | Mitigation |
|---|---|
| Windows' own per-arrival cost (WM_DISPLAYCHANGE, DWM swap-chain re-creation) exceeds 250 ms at 8 displays, whatever the driver does | Measure it with the third-party drivers first (this run). Arrive monitors back to back in one request; if needed, report them only after the host has set the topology. If it still misses the target, record that as a platform limit in perf.md with numbers, and ask you rather than hide it. **Outcome:** it did; the target was split into steady state and per-change freezes (ADR 0006). |
| The dev PC crashes on its own (0x1C7/0x3B/0x1E earlier), which muddies stress results | Resumable harness and crash tagging. Repeat any run that crashed; separate the effect of a crash from the driver in perf.md. A memory test or EXPO off would remove the doubt (your call). |
| NVIDIA-specific behaviour (Apollo #179 periodic stutter with a DP monitor) | The hold run detects periodic stalls; results are recorded per GPU. An explicit render adapter avoids cross-GPU copies. |
| IddCx differences between Windows 10 and 11 (HDR needs 1.10) | Version-gated features, with the HDR path off when unavailable. CI builds against the WDK's IddCx headers; runtime checks use `IDD_IS_FUNCTION_AVAILABLE`. |
| Handle-ownership grace period removes monitors during a host restart | 2 s grace period plus re-claim by ID; the stress test covers a service restart. |

## Decisions needed from you

1. Approve this design (or say what to change).
2. Is the "monitors belong to the handle, removed 2 s after it closes" behaviour right? The alternative is that
   monitors stay until explicitly removed, which risks leaked displays after a host crash.
