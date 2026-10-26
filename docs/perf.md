# Performance results

Targets from the brief (§4.5). Results are added as they are measured, with date, build, hardware and method.

| Target | Limit | Result | Date / build / hardware |
|---|---|---|---|
| Extra latency per additional display (LAN, 1080p60) | ≤ 2 ms | — | — |
| Add or remove one virtual display | < 3 s | — | — |
| Longest system-wide stall during add/remove (1, 2, 4, 8 displays) | ≤ 250 ms | — | — |
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
