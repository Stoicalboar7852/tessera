# vdd-stress

Adds and removes virtual displays in cycles, times every change, and watches for system-wide stalls meanwhile. In
Phase 1 it reproduces the multi-display freeze with third-party drivers (`docs/vdd-freeze-analysis.md`). Once Tessera's
driver has a backend, it becomes that driver's acceptance test (brief §4.5).

| File | What it is |
|---|---|
| `src/` | `vdd-stress.exe` (C++20, MSVC and MinGW). Backends: `sudovda`, `parsec`, `mtt` |
| `third-party-vdds.json` | The pinned third-party packages: sources and SHA-256 of every file |
| `Get-VddPackages.ps1` | Downloads and unpacks those packages into `<project>\cache\vdd-repro` (no elevation) |
| `Run-VddRepro.ps1` | The elevated, unattended, resumable reproduction run for all three drivers |
| `tessera-display.wprp` | WPR profile with IddCx, Display, DisplaySwitch, Kernel-PnP and UMDF events |

## vdd-stress.exe

```
vdd-stress run   --backend sudovda|parsec|mtt [--counts 1,2,4,8] [--cycles 10] [--mode 1920x1080@60]
                 [--hold-ms 2000] [--settle-ms 500] [--timeout-ms 10000] [--out DIR]
vdd-stress hold  --backend B --count N --seconds S [--out DIR]
vdd-stress probe --seconds S [--out DIR]          baseline without any driver (no elevation needed)
vdd-stress save FILE | restore FILE               display layout as JSON
```

- **What it measures:**
  - **Change time:** from the driver call until the CCD topology shows the display as active (add) or gone (remove).
  - **Stalls:** three probes run during the whole run. `compose` redraws a 4×4 topmost window in the bottom-right
    corner of the primary display and waits with `DwmFlush`. `ui` sends `WM_NULL` round trips to Explorer's taskbar.
    `sched` is a time-critical thread sleeping 1 ms. Gaps longer than `--record-ms` (default 50) are kept, with the
    operation they overlapped.
  - **CPU:** dwm.exe, WUDFHost.exe and explorer.exe, once per second.
- **Output:** `events.csv`, `gaps.csv`, `cpu.csv` and `summary.json`. The summary includes `targets_met` against the
  brief's limits (each change < 3 s, no stall > 250 ms, no hang). Exit code 0 means the targets were met, 1 missed or
  failed, 2 usage error, 3 aborted.
- **Clean-up:** displays it added are removed and the layout from before the run is restored, also after Ctrl+C. If
  the process is killed, SudoVDA and parsec-vdd drop their displays on their own (keepalive). MTT keeps them until
  its device is disabled.

## Reproduction run

```
.\tools\vdd-stress\Get-VddPackages.ps1                     # once, unelevated
powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\vdd-stress\Run-VddRepro.ps1"   # elevated
```

The run takes about 45 minutes. Output goes to `<project>\logs\vdd-repro\<timestamp>\`, with `results.md` as the
overview. See the script header for exactly what it changes on the machine and how it undoes it.
