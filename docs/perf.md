# Performance results

Targets from the brief (§4.5). Results are added as they are measured, with date, build, hardware and method.

| Target | Limit | Result | Date / build / hardware |
|---|---|---|---|
| Extra latency per additional display (LAN, 1080p60) | ≤ 2 ms | — | — |
| Add or remove one virtual display | < 3 s | **Met with the physical monitors on** (runs 1 and 2: 3,200 one-at-a-time changes with 8 displays, worst 2.33 s). Batched adds of 8 at once: worst 3.15 s. **Headless: not met in run 2** (removes up to 4.3 s, batched adds of 8 up to 6.2 s), because Windows switched the physical displays back on during most changes. Run 3 measures it with stable monitor IDs and switches them off again, as a host would. | 2026-10-27/29, VDD 0.2.0, dev PC |
| Longest system-wide stall during add/remove (1, 2, 4, 8 displays) | ≤ 250 ms | **Not met.** Every add/remove stalls Explorer and DWM, typically 0.3–0.7 s per change with 1–2 displays and 1–1.3 s for a batch of 8, and far longer when changes come faster than Windows handles them. Same with SudoVDA, parsec-vdd and MTT. With 4 displays held for 120 s, nothing stalls over 250 ms. Runs 1 and 2 gave every add a never-seen monitor; run 3 repeats it with stable IDs. | 2026-10-27/29, VDD 0.2.0, dev PC |
| Encoder session limit per GPU vendor | reported to the user | — | — |
| Game Mode resource savings (RAM, CPU, background processes) | measured | — | — |

## Reference hardware

| Machine | CPU / GPU | Displays | Notes |
|---|---|---|---|
| Dev PC | Ryzen 5 7600X (AMD iGPU) + NVIDIA RTX 4060 Ti (driver 32.0.16.1088), 31 GB RAM | Dell P2417H (1920×1080@60), H32S17 (2560×1440@59.95) | Windows 11 IoT Enterprise LTSC 2024 (26100) since 2026-10-26; before: Windows 11 Pro 26200 |

## Stall-probe baseline (no virtual display driver)

`vdd-stress probe` (see `tools/vdd-stress/README.md`), dev PC, 2026-10-26, 15–20 s, two physical displays:

| Probe | Longest interval | Meaning |
|---|---|---|
| compose (DwmFlush) | 18 ms | one 60 Hz frame |
| ui (Explorer WM_NULL) | 0.4 ms | |
| sched (1 ms sleep, time-critical) | 2.9 ms | |

## Tessera VDD: first acceptance run (2026-10-27)

`tools/vdd-stress/Run-VddAcceptance.ps1`, dev PC, two physical monitors on, 1920×1080@60 virtual displays, VDD
0.2.0. Raw data: `logs\vdd-accept\20261027-0651`. Change time is measured from the driver call until the display is
active (or gone) in the CCD topology.

| Run | Change time, median / p95 / max | Worst stall: compose / Explorer UI | Failures |
|---|---|---|---|
| add with 1 / 2 / 4 / 8 displays (20 cycles each) | 135 / 256 / 391 / 567 ms median; max 159 / 501 / 654 / 2,586 ms | up to 52 s / 10 s (see below) | 0 |
| remove with 1 / 2 / 4 / 8 displays | 69 / 261 / 405 / 436 ms median; max 913 ms | | 0 |
| 8 displays × 100 cycles, one at a time | add 600 / 1,255 / 2,330 ms; remove 429 / 719 / 1,108 ms | Explorer unresponsive for most of the run | 0 of 1,600 |
| 8 displays × 100 cycles, all 8 in one request | add 1,744 / 2,487 / 3,151 ms (for all 8); remove 230 / 369 / 387 ms | 2.0 s / 3.9 s | 0 of 200 |
| 2 HDR displays × 10 cycles | add 532 / 636 / 669 ms; remove 369 / 511 / 608 ms | 1.1 s / 1.3 s | 0 |
| 2 displays at 59.94 Hz × 10 cycles | add 474 / 768 / 788 ms; remove 488 / 497 / 499 ms | 0.8 s / 2.1 s | 0 |

**Compared with the third-party drivers on the same PC** (`docs/vdd-freeze-analysis.md`):
- SudoVDA's add time grows from 154 to 1,109 ms median with 1 to 8 displays (worst 11.6 s); Tessera's grows from 135
  to 567 ms (worst 2.6 s).
- MTT's freezes come from its driver (89 % of a core with idle displays); Tessera's driver host stays near 0 % CPU.

**Why the stall target is not met.** In the one-at-a-time 8×100 run:
- the Explorer probe hit its 10 s timeout on almost every call, and the compose probe got only 16 samples in 18
  minutes;
- the scheduler probe never exceeded 5 ms, so the CPU was free: Explorer and DWM were busy reconfiguring.

With 10 displays, each monitor arrival makes Windows re-evaluate the topology, rebuild a taskbar per display and
re-create DWM's swap chains. Changes every ~0.6 s queue up faster than Explorer finishes them. Batching all 8
displays into one request cuts the worst stalls to 2.0 s (compose) and 3.9 s (Explorer), but each burst still
stalls for 0.3–2 s.

**Not valid in this run:**
- The headless steps: the harness counted active paths, and Windows switches the physical displays back on when a new
  monitor arrives, so every wait timed out. The harness now waits for the specific display.
- The driver uninstall at the end seemed to hang and left the device stuck as "being removed" until a reboot. The
  first guess was the driver's D3D device lifetime (it now releases each GPU's device with the last swap chain). The
  second run found the real cause, below.

## Tessera VDD: second acceptance run (2026-10-29)

Same PC, method and driver, with the fixed harness. Raw data: `logs\vdd-accept\20261029-1744`. Every one of the
3,560 changes succeeded, with no timeout. "Worst stall per change" is the longest compose or Explorer probe gap
from the start of a change to the start of the next one.

| Run | Change time, median / p95 / max | Worst stall per change, median / p90: compose, Explorer |
|---|---|---|
| add with 1 / 2 / 4 / 8 displays (20 cycles each) | 209 / 306 / 465 / 571 ms median; max 1,256 ms | 0.5 / 7.1 s, 0.7 / 9.7 s |
| 8 displays × 100 cycles, one at a time | add 503 / 968 / 1,394 ms; remove 432 / 696 / 1,193 ms | 27 / 58 s, 10 / 10 s (probes starved) |
| 8 displays × 100 cycles, all 8 in one request | add 1,873 / 2,617 / 2,663 ms; remove 324 / 391 / 443 ms | 1.3 / 1.4 s, 1.1 / 1.3 s |
| headless, 8 × 100, one at a time | add 846 / 1,455 / 2,168 ms; **remove 686 / 3,380 / 4,348 ms** | 0.9–2.3 / 4–6 s, 3–6 / 10 s |
| headless, 8 × 100, all 8 in one request | **add 2,105 / 4,374 / 6,175 ms**; remove 480 / 3,143 / 3,740 ms | 1.9 / 2.9 s, 1.5–2 / 4 s |
| 2 displays at 59.94 Hz / 2 HDR displays, × 10 | add 459–470 ms median, max 739 ms | 0.3 / 0.3–0.4 s, 0.4 / 0.6–0.9 s |
| 4 displays held for 120 s | — | no stall over 250 ms outside the adds and removes |

**Findings:**
- **The uninstall did not hang.** Each add used a new random monitor ID, so Windows created a new monitor device and
  new saved display settings every time. The two runs left 4,336 monitor devices and about 6,900 settings entries
  behind (`GraphicsDrivers\ScaleFactors`, `MonitorDataStore`, and likely `Configuration`). Removing the adapter
  deletes its monitor devices one by one, at about 150 ms each. The harness killed it after 5 minutes, both times,
  which left the adapter stuck.
- **Every add was a first connection.** A host gives each client monitor a stable ID, so in normal use Windows sees
  the same monitors again. vdd-stress now does the same: one stable ID per display slot. `--new-ids` and the new
  `newids-8x20` step keep the first-connection case.
- **Headless:** Windows applied its default "extend" layout for each new set of monitors, which switched the physical
  displays back on in 1,500 of 1,600 changes. The removes over 3 s include Windows switching the physical monitors
  on. vdd-stress only counted this. It now switches them off again, as a headless host must, and reports how long
  that takes. With stable IDs, Windows should also remember the layout per set of monitors.
- **Explorer crashed four times:** once in the per-count step and three times in the headless one-at-a-time step,
  each time in `taskbar.dll` with exception `0xc000027b` (a stowed WinRT exception). Windows restarted it each time.
  Run 1 logged no crash.
- **Taskbar experiment: inconclusive.** With "show taskbar on all displays" off, the stalls were much lower: worst
  stall per change 0.8 s compose and 1.1 s Explorer (median), against 27 s and 10 s with it on. But from 18:26 the
  console display was off (15-minute idle timeout) and the session was locked for part of the time. That covers the
  experiment steps, the second half of the headless step, and the 120 s hold. Run 3 keeps the displays on and records
  locks and display-off.
- **Harness stall:** after `wpr -stop` hung for 15 minutes in the hold step, the harness sat idle for two hours until
  someone touched the PC. The likely cause is a click in its console window: QuickEdit pauses console output. The
  harness now turns QuickEdit off.

**Changed for run 3:** stable monitor IDs, plus the `newids-8x20` step. In headless runs, vdd-stress switches the
physical displays off again after each change. The run keeps the displays on and records session lock, display-off and
remote sessions. `vdd-install forget-monitors` deletes leftover test monitors and their settings, before and after the
run, after exporting `GraphicsDrivers` to the run folder. The uninstall timeout is now 30 minutes.
