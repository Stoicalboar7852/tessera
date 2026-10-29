# vdd-stress

Adds and removes virtual displays in cycles, times every change, and watches for system-wide stalls meanwhile. In
Phase 1 it reproduced the multi-display freeze with third-party drivers (`docs/vdd-freeze-analysis.md`). With the
`tessera` backend it is the acceptance test for Tessera's own driver (brief §4.5).

| File | What it is |
|---|---|
| `src/` | `vdd-stress.exe` (C++20, MSVC and MinGW). Backends: `tessera`, `sudovda`, `parsec`, `mtt` |
| `Run-VddAcceptance.ps1` | The elevated, unattended, resumable Phase 1 acceptance run for Tessera's driver |
| `third-party-vdds.json` | The pinned third-party packages: sources and SHA-256 of every file |
| `Get-VddPackages.ps1` | Downloads and unpacks those packages into `<project>\cache\vdd-repro` (no elevation) |
| `Run-VddRepro.ps1` | The elevated, unattended, resumable reproduction run for the three third-party drivers |
| `tessera-display.wprp` | WPR profile with IddCx, Display, DisplaySwitch, Kernel-PnP and UMDF events (ring buffer) |

## vdd-stress.exe

```
vdd-stress run   --backend tessera|sudovda|parsec|mtt [--counts 1,2,4,8] [--cycles 10] [--mode 1920x1080@60]
                 [--hold-ms 2000] [--settle-ms 500] [--timeout-ms 10000] [--out DIR]
                 [--batch] [--headless] [--hdr] [--new-ids]
vdd-stress hold  --backend B --count N --seconds S [--out DIR]
vdd-stress probe --seconds S [--out DIR]          baseline without any driver (no elevation needed)
vdd-stress save FILE | restore FILE               display layout as JSON
```

- **Options:**
  - `--batch` adds all displays of a cycle in one driver request and removes them in one (tessera only). It shows
    what batching saves compared with one display at a time.
  - `--headless` adds one extra "anchor" display, then switches the physical displays off through the state journal
    (`journaled_display_change`), so `recover.exe --restore` can undo it if the run dies. It switches them back on at
    the end. Windows applies its saved (or default "extend") layout for each new set of monitors, which can switch the
    physical displays back on. As a headless host must, vdd-stress then switches them off again; the summary counts
    how often (`physical_reactivations`) and how long that took (`headless_reapply_ms`).
  - `--hdr` adds HDR10 monitors (tessera, driver on IddCx 1.10 or later).
  - **Monitor IDs (tessera).** Each display slot gets the same monitor ID in every cycle and run, as a host gives each
    client monitor a stable ID. Windows then sees the same few monitors again. `--new-ids` uses a never-seen ID for
    every add instead: the first-connection case. Every new ID leaves a monitor device and saved display settings
    behind, and the first two acceptance runs left about 7,000 of them. `vdd-install forget-monitors --settings`
    deletes them.
- **What it measures:**
  - **Change time:** from the driver call until the CCD topology shows the display as active (add) or gone (remove).
  - **Stalls:** three probes run during the whole run. `compose` redraws a 4×4 topmost window in the bottom-right
    corner of the primary display and waits with `DwmFlush`. `ui` sends `WM_NULL` round trips to Explorer's taskbar.
    `sched` is a time-critical thread sleeping 1 ms. Gaps longer than `--record-ms` (default 50) are kept, with the
    operation they overlapped.
  - **CPU:** dwm.exe, WUDFHost.exe and explorer.exe, once per second.
  - **Conditions:** it keeps the displays on while it runs (no idle display-off or screen saver). The summary's
    `environment` records how long the session was locked, the console display was off or the session was remote,
    and `ops_while_away` counts the operations affected. Windows and Explorer handle display changes differently
    then, so those measurements are not comparable.
- **Output:** `events.csv`, `gaps.csv`, `cpu.csv` and `summary.json`. The summary includes `targets_met` against the
  brief's limits (each change < 3 s, no stall > 250 ms, no hang). Exit code 0 means the targets were met, 1 missed or
  failed, 2 usage error, 3 aborted.
- **Clean-up:** displays it added are removed and the layout from before the run is restored, also after Ctrl+C. If
  the process is killed:
  - Tessera's driver removes its monitors 2 s after the handle closes.
  - SudoVDA and parsec-vdd drop their displays on their own (keepalive).
  - MTT keeps them until its device is disabled.

## Acceptance run (Tessera VDD, dev PC)

```
powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\vdd-stress\Run-VddAcceptance.ps1"   # elevated
```

Needs the driver package built and signed (`tools/signing/Sign-DriverPackage.ps1`) with a certificate trusted by
`tools/setup/admin-once.ps1`. The steps take about 70 minutes (run 3), plus the clean-up of earlier test monitors
(about 150 ms per leftover monitor device, before and after). Output goes to `<project>\logs\vdd-accept\<timestamp>\`, with
`results.md` as the overview. The script header lists the steps. Don't lock the PC while it runs.

`per_burst.py <run folder> [steps...]` gives the worst stall per burst (one batch, or one cycle's one-at-a-time
changes), as reported in `docs/perf.md` from run 3 on.

## Reproduction run (third-party drivers)

```
.\tools\vdd-stress\Get-VddPackages.ps1                     # once, unelevated
powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\vdd-stress\Run-VddRepro.ps1"   # elevated
```

The run takes about 45 minutes. Output goes to `<project>\logs\vdd-repro\<timestamp>\`, with `results.md` as the
overview. See the script header for exactly what it changes on the machine and how it undoes it.
