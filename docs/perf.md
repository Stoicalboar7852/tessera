# Performance results

Targets from the brief (§4.5). Results are added as they are measured, with date, build, hardware and method.

| Target | Limit | Result | Date / build / hardware |
|---|---|---|---|
| Extra latency per additional display (LAN, 1080p60) | ≤ 2 ms | — | — |
| Add or remove one virtual display | < 3 s | **Met** in run 3 (5,148 changes, stable monitor IDs): worst 1.86 s with the physical monitors on, 2.58 s headless. A batch of 8 takes at most 0.74 s. **Exception:** headless, one at a time, Windows switched the physical displays back on after half the adds; switching them off again made 26 of 800 adds take over 3 s in total (worst 3.9 s). It never happened with batches. | 2026-10-29, VDD 0.2.0, dev PC |
| Longest system-wide stall during add/remove (1, 2, 4, 8 displays) | ≤ 250 ms; split by ADR 0006 into steady state (≤ 250 ms) and one freeze per change within the run-3 baseline | **Steady state: met. During changes: 250 ms is not met, and run 3 sets the baseline.** Each change freezes DWM and Explorer while Windows reconfigures: one display about 0.2 s (80–85 % of single changes stay under 250 ms; worst 0.34 s compose, 0.48 s Explorer), two displays 0.2–0.5 s, a batch of 8 about 0.5–1 s (worst 1.6 s; Explorer up to 3.2 s headless). Changes made back to back queue up and freeze the desktop for seconds. From 1 s after a change, nothing stalls over 250 ms. It is Windows' own cost; SudoVDA, parsec-vdd and MTT stall the same or worse. | 2026-10-29, VDD 0.2.0, dev PC |
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

## Tessera VDD: third acceptance run (2026-10-29)

Same PC, method and driver, with stable monitor IDs (except `newids-8x20`). Raw data: `logs\vdd-accept\20261029-2207`.
All 5,148 changes succeeded. No step had a session lock, display-off or remote session; PowerToys Awake was also on.

**Clean-up:** before the run, `forget-monitors` removed the 4,336 leftover monitor devices in 9 minutes and deleted
about 17,600 saved settings. At the end it removed the 169 monitors this run created (160 from `newids-8x20`), and the
adapter uninstalled in about 1 s. The steps took 70 minutes.

**Stall metric.** Run 2's "per change" metric counted one long gap once for every change it overlapped. This run
counts per **burst**: one batched request, or one cycle's 8 one-at-a-time adds (or removes). A burst owns the time
until the next burst starts, so the 1 s hold after it counts too. Run 2 values below use the same metric.

| Run | Change time, median / p95 / max (run 2) | Worst stall per burst, median / p90: compose; Explorer (run 2 medians) |
|---|---|---|
| 1 display (20 cycles) | add 89 / 171 / 205 ms; remove 39 / 140 / 148 ms | add 0.23 / 0.27 s; 0.16 / 0.23 s (0.27; 0.51). Remove 0.20 / 0.32 s; 0.13 / 0.24 s |
| 2 displays one at a time (per-count, 59.94 Hz, HDR) | add 196–228 ms median, max 495 ms | add 0.36–0.49 s; 0.27–0.31 s. Remove 0.22–0.24 s; 0.12–0.14 s |
| 4 displays one at a time | add 313 / 556 / 587 ms; remove 225 / 416 / 434 ms | add 1.1 / 1.4 s; 0.53 / 1.1 s. Remove 0.44 / 0.68 s; 0.43 / 1.1 s |
| 8 × 100, one at a time | add 403 / 1,081 / 1,861 ms (503 / 968 / 1,394); remove 353 / 642 / 900 ms (432 / 696 / 1,193) | 6.9 / 16 s; 2.0 / 10 s (29; 10). Composition stalled 99 % of the step |
| 8 × 100, all 8 in one request | **add 512 / 550 / 678 ms (1,873 / 2,617 / 2,663)**; remove 116 / 162 / 242 ms (324 / 391 / 443) | add 0.93 / 1.24 s; 0.56 / 0.77 s (1.32; 1.11). Remove 0.54 / 1.24 s; 0.54 / 0.77 s |
| headless, 8 × 100, one at a time | add 590 / 1,594 / 2,578 ms (846 / 1,455 / 2,168); remove 383 / 628 / 732 ms (686 / 3,380 / 4,348) | composition stalled for the whole step (longest gap 286 s); Explorer at its 10 s timeout |
| headless, 8 × 100, all 8 in one request | **add 525 / 639 / 687 ms (2,105 / 4,374 / 6,175)**; remove 123 / 210 / 288 ms (480 / 3,143 / 3,740) | add 1.05 / 1.42 s; 0.74 / 1.91 s (1.87; 2.04). Remove 0.90 / 1.42 s; 0.61 / 2.04 s |
| 8 × 20, one at a time, new monitor IDs | add 516 / 1,014 / 1,250 ms; remove 418 / 647 / 734 ms. Stable IDs, first 20 cycles: 432 / 1,094 / 1,858 and 344 / 619 / 709 ms | 3.3 / 4.4 s; 2.0 / 6.6 s |
| 4 displays held for 120 s | — | from 1 s after the last add, no compose gap recorded and Explorer at most 218 ms |

**Findings:**
- **Stable IDs help batches most.** Batched adds of 8 dropped from 1.87 s to 0.51 s median, and their stalls by
  30–50 %. One at a time, a never-seen monitor costs about 80 ms more per add than a known one. The uninstall takes a
  second instead of hanging.
- **One freeze per change.** A single display change freezes composition for about 0.2 s and Explorer for about
  0.15 s; that is close to the target, but not under it (20 % of single adds and 15 % of removes exceed 250 ms). Each
  further display in the same change adds less than a separate change would: 8 in one request freeze for about 1 s.
- **Back-to-back changes queue up.** vdd-stress starts the next change as soon as the previous one shows in the
  topology, but DWM and Explorer are still reconfiguring. Eight one-at-a-time changes then freeze the desktop for
  seconds, and the stall carries into the next cycle. The scheduler probe stayed under 7 ms throughout, so the CPU was
  free. A host must apply a client's displays as one request and must not chain changes.
- **Headless works best batched.** In the batched step Windows never switched the physical displays back on. One at a
  time, it did so after 410 of 800 adds (never after a remove): after every add in the first 10 cycles, then after
  about a quarter of them, as Windows remembered the layouts. Switching them off again took 0.9 s median, 1.9 s at most, and is
  another layout change: in that step the compose probe was starved for all 23 minutes.
- **Explorer crashed once** (physical, one at a time; `taskbar.dll`, `0xc000027b`, as in run 2), against four times
  in run 2.
- **Taskbar experiment: no consistent benefit.** With "show taskbar on all displays" off, batched stalls were the
  same for composition (0.94 against 0.93 s) and a little worse for Explorer (0.70 against 0.56 s). One at a time,
  Explorer answered faster (0.67 s against 2.0 s in both steps with the taskbar on), but composition was stalled for
  93–99 % of the step either way. It isn't worth changing a user setting for.
