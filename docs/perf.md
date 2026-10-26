# Performance results

Targets from the brief (§4.5). Results are added as they are measured, with date, build, hardware and method.

| Target | Limit | Result | Date / build / hardware |
|---|---|---|---|
| Extra latency per additional display (LAN, 1080p60) | ≤ 2 ms | — | — |
| Add or remove one virtual display | < 3 s | **Met** with physical monitors on: 1,600 one-at-a-time changes with 8 displays, worst add 2.33 s, worst remove 1.11 s. Batched adds of 8 at once: worst 3.15 s. Headless: not measured yet (harness bug). | 2026-10-27, VDD 0.2.0, dev PC |
| Longest system-wide stall during add/remove (1, 2, 4, 8 displays) | ≤ 250 ms | **Not met.** Every add/remove stalls Explorer and DWM for 0.3–2 s, and longer when changes come faster than Windows handles them. Same with SudoVDA, parsec-vdd and MTT. See below. | 2026-10-27, VDD 0.2.0, dev PC |
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
- The driver uninstall at the end hung and left the device stuck as "being removed" until a reboot. Probable cause,
  not yet confirmed: the driver kept its D3D device until it unloaded, past the swap chains it served; it now
  releases the device with the last swap chain. The GPU-less VM never assigns swap chains, so only the dev PC can
  confirm this.

A second run with the fixed harness and driver, plus an experiment with "show taskbar on all displays" switched off,
follows.
