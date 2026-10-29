# Phase 1 summary: virtual display driver

_Completed and confirmed 2026-10-30. Details are in `docs/STATUS.md`, `docs/perf.md`,
`docs/vdd-freeze-analysis.md`, `docs/phase-1-design.md` and ADRs 0005–0006._

## Acceptance

Brief §8: "8 displays added and removed 100 times in a stress script, meeting the §4.5 targets, headless and with
physical monitors." Results are from the third dev-PC run (2026-10-29, `docs/perf.md`) unless noted.

| Criterion | Result | Evidence |
|---|---|---|
| Reproduce and root-cause the freeze with the existing drivers first (F1) | ✅ | `docs/vdd-freeze-analysis.md`: MTT's single worker thrashes (89 % of a core with idle displays), SudoVDA's add cost grows with the display count, parsec-vdd starts refusing adds; Windows has its own per-change floor |
| 8 displays × 100 add/remove, physical monitors on | ✅ | 1,600 one-at-a-time and 200 batched changes, none failed |
| The same, headless | ✅ | 1,600 one-at-a-time and 200 batched changes, none failed |
| Each add or remove < 3 s | ✅, one exception | Worst 1.86 s with the physical monitors on, 2.58 s headless; a batch of 8 takes at most 0.74 s. Headless and one at a time, 26 of 800 adds took over 3 s once switching the physical monitors back off is counted. The host batches (ADR 0006), and batches never triggered it. |
| No hang | ✅ | Over 10,000 changes in three runs. The uninstall takes about 1 s; the earlier "hang" was leftover test monitors. |
| No system-wide stall > 250 ms | Split by ADR 0006. Steady state ✅. During changes, Windows' freeze is accepted, with run 3 as the baseline. | `hold-4`: nothing over 250 ms from 1 s after a change. One display freezes for about 0.2 s, 8 in one request for about 1 s. |
| 1, 2, 4 and 8 displays | ✅ | `per-count` step |
| Per-monitor modes, including 59.94 Hz, and HDR on/off | ✅ | `mode-5994` and `hdr` steps; 11 unit tests for CVT-RB timings, mode lists and EDID |
| Zero physical monitors (F1) | ✅ | headless steps |
| Dev-signed driver with Secure Boot on (§4.4) | ✅ | `tools/vm/Test-VddMonitorsInVm.ps1` in the Secure Boot VM, plus the dev PC |

**Not verified:**
- `SET_RENDER_ADAPTER` is implemented, but nothing calls it yet; the runs used Windows' choice of render GPU. The
  Phase 2 host sets the encoding GPU and will exercise it.
- Re-claiming monitors by ID within the 2 s grace period (a host service restart). The VM test only covers removal
  after the grace period.
- 119.88 Hz on real hardware (unit-tested only), Windows 10 or IddCx below 1.10 (the HDR-off path), and the frame
  workers recovering from a GPU reset or a GPU driver update.
- Windows remembering position and DPI per stable ID. This was seen indirectly: in the headless run, Windows
  re-enabled the physical monitors after every add at first, then after about a quarter of them, as it learned the
  layouts. It wasn't checked directly.
- Phase 0's manual checks are still open: a layout restored by the service after a reboot, the emergency hotkey, and
  a local MSVC build.

## What exists now

- **Driver** (`drivers/vdd`, VDD 0.2.0; UMDF 2.25 with IddCx 1.10, 1.4 minimum; GPLv3, written from the documentation
  per ADR 0005):
  - adds and removes 1–16 monitors per request at runtime, without restarting the adapter;
  - control interface v2: `GET_VERSION`, `SET_RENDER_ADAPTER`, `ADD_MONITORS`, `REMOVE_MONITORS` and
    `LIST_MONITORS`; admin-only, with every request validated;
  - one event-driven worker per monitor (MMCSS, no polling, no I/O on the frame path), and one D3D device per GPU,
    released with its last swap chain;
  - monitors belong to the handle that added them, are removed 2 s after it closes, and can be re-claimed by ID;
  - a generated EDID (manufacturer "TSR"; serial, product code and container ID from the stable ID), at most 6 modes
    per monitor, exact fractional refresh rates, and HDR only where requested.
- **Shared code** (`host-helpers/common`): `tessera/display_modes.h` (timings, mode lists, EDID) and
  `tessera/vdd_client.h` (control client), compiled into the driver and the tools.
- **Tools:**
  - `vdd-stress`: Tessera and third-party backends; one-at-a-time or batched, headless (with layout re-apply), HDR,
    stable or new monitor IDs; stall probes and run conditions;
  - `Run-VddAcceptance.ps1`: the full matrix, unattended and elevated, about 1.5 h, cleaning up before and after;
  - `per_burst.py`: the stall metric of ADR 0006;
  - `vdd-install`: install, remove and query, plus `forget-monitors`;
  - `Test-VddMonitorsInVm.ps1`, `Run-VddRepro.ps1` and `Get-VddPackages.ps1`.
- **CI** builds the driver and the native components (MSVC and MinGW) on every push.

## Findings worth knowing

1. **Windows freezes composition and Explorer on every display change**: about 0.2 s for one display, about 1 s for
   8 at once, whatever the driver. ADR 0006 accepts this, with a baseline.
2. **Back-to-back changes queue up** into freezes of several seconds. The host must batch and never chain changes.
3. **Every new monitor ID leaves a monitor device and saved settings behind**, and Windows takes about 150 ms each to
   delete them. Two runs with random IDs left 4,336, enough to make the uninstall look hung. Stable IDs also make a
   batched add of 8 3.6 times faster (0.51 s against 1.87 s).
4. **Headless:** for a set of monitors it hasn't seen, Windows applies its default "extend" layout and switches the
   physical monitors back on. The host must re-apply its layout and save it.
5. **Explorer can crash in `taskbar.dll`** (`0xc000027b`) under heavy churn: four times in run 2 and once in run 3.
   Windows restarts it. Turning off "show taskbar on all displays" doesn't help consistently.
6. **The dev PC was stable for the runs:** one unexpected restart since the reinstall (2026-10-27 00:35, no bugcheck
   recorded), and none during the stress runs.

## Next: Phase 2 (protocol and host multi-stream)

Capability negotiation, a multi-video-stream extension in common-c and the host, one capture → encode pipeline per
display, and a bandwidth split. *Acceptance:* a test client receives 2–4 streams, and stock Moonlight still works.
The host drives the driver through `vdd_client` under the ADR 0006 rules: batched display changes, a stable ID per
client monitor, and the encoding GPU as render adapter. A design for Phase 2 comes first, for approval.
