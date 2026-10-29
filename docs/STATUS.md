# Project status: Tessera

_Last updated: 2026-10-30_

| Phase | State | Notes |
|---|---|---|
| 0 – Research and setup | **Done** | Both acceptance criteria are met, and the driver and watchdog are verified in a Secure Boot VM. Summary: `docs/phase-0-summary.md`. |
| 1 – VDD | **Done, waiting for confirmation.** Three dev-PC runs did over 10,000 changes with no failure. With stable monitor IDs (run 3) every change takes under 3 s, except some headless one-at-a-time adds where Windows switches the physical monitors back on. The stall target is split (ADR 0006): steady state ≤ 250 ms is met, and display changes have a measured baseline. | Summary: `docs/phase-1-summary.md`. See also "Phase 1 progress" and `docs/perf.md`. |
| 2–12 | Not started | |

## Phase 1 progress
- **Freeze analysis** (`docs/vdd-freeze-analysis.md`): source and issue-tracker review, plus measurements on the
  dev PC. The freeze reproduces with all three drivers: compose stalls of up to 77 s (SudoVDA) and 121 s (MTT), and
  MTT's host uses 89 % of a core with 4 idle displays. Windows itself costs up to ~300 ms of compositor stall per
  add, whatever the driver does.
- **Design** approved on 2026-10-27 (`docs/phase-1-design.md`), including handle-owned monitors that are removed
  2 s after the handle closes. Licence: ADR 0005.
- **Driver verified in the Secure Boot VM** (2026-10-27, `tools/vm/Test-VddMonitorsInVm.ps1`: PASS, Windows 11 Pro
  26300, no GPU):
  - installs dev-signed with Secure Boot on;
  - adds and removes 1, 2 and 4 monitors one at a time and batched, every operation successful; changes are
    visible in 26–237 ms;
  - after a holder process is killed, its 2 monitors are removed in 2.07 s (the 2 s grace period);
  - no WUDFHost crash or UMDF host failure.

  The VM composes in software (about 280 ms stalls even when idle), so its timings are not performance results.
  **Windows never assigns swap chains in the GPU-less VM**, so the VM test covers the monitor lifecycle and
  ownership, but not the frame workers or the D3D device cache. CI builds the driver, the native components (MSVC
  and MinGW) and the host.
- **First dev-PC acceptance run** (2026-10-27, `docs/perf.md`):
  - all 1,600 one-at-a-time changes with 8 displays succeed in under 3 s (worst add 2.33 s);
  - batched adds of 8 take at most 3.15 s;
  - stalls are far above 250 ms: Explorer stays unresponsive while changes queue up, and is better but still
    0.3–2 s per burst when batched;
  - the headless steps were invalid (harness bug, fixed);
  - the final uninstall hung, leaving the device stuck "being removed" until a reboot. Probable fix: release each
    GPU's D3D device with the last swap chain.
- **Since the first run:**
  - `vdd-stress` waits for the exact display target (from the driver), wakes on `WM_DISPLAYCHANGE` instead of
    polling, and counts how often Windows switches physical displays back on in headless runs;
  - the WPR profile records the driver's own events;
  - the acceptance run adds a "taskbar on all displays off" experiment;
  - the VM test also runs HDR monitors and a timed uninstall.
- **Driver** (`drivers/vdd`, IddCx 1.10, 1.4 minimum): builds with the EWDK at `/W4 /WX` and is dev-signed. It has:
  - runtime add/remove of up to 16 monitors over interface v2;
  - one event-driven worker per monitor and one D3D device per GPU;
  - handle ownership with a 2 s grace period;
  - at most 6 modes per monitor, a generated EDID, and HDR only where requested.
- **Shared code:** `tessera/display_modes.h` (CVT-RB timings, mode lists, EDID; 11 unit tests) and
  `tessera/vdd_client.h` (control client).
- **Tools:**
  - `vdd-stress` has a `tessera` backend and the `--batch`, `--headless` and `--hdr` options;
  - `Run-VddAcceptance.ps1` runs the acceptance matrix on the dev PC (elevated, about 1.5 h);
  - `Run-VddRepro.ps1` and `Get-VddPackages.ps1` cover the third-party reproduction;
  - `tools/vm/Test-VddMonitorsInVm.ps1` checks the driver in the Secure Boot VM.
- **Second dev-PC acceptance run** (2026-10-29, `docs/perf.md`):
  - all 3,560 changes succeed;
  - the stalls look the same as in run 1;
  - steady state with 4 displays has no stall over 250 ms;
  - headless removes take up to 4.3 s, because Windows switched the physical monitors back on;
  - Explorer crashed 4 times in `taskbar.dll`;
  - the taskbar experiment is inconclusive, because the display went off on its idle timeout.

  The "uninstall hang" was not the driver. Every add used a new random monitor ID, so about 4,300 leftover monitor
  devices built up, and Windows deletes them at about 150 ms each when the adapter is removed.
- **Since the second run:**
  - `vdd-stress` uses one stable ID per display slot (`--new-ids` for the old behaviour). In headless runs it
    switches the physical monitors off again after each change, as a host must. It keeps the displays on and records
    locks and display-off.
  - New `vdd-install forget-monitors [--settings]` deletes leftover test monitors and their saved display settings.
  - The acceptance harness cleans up before and after a run, allows 30 minutes for the uninstall, turns QuickEdit off,
    and adds a `newids-8x20` step.
- **Third dev-PC acceptance run** (2026-10-29, `docs/perf.md`), with stable monitor IDs:
  - all 5,148 changes succeed; the clean-up removed the 4,336 leftover monitors, and the uninstall took about 1 s;
  - batched adds of 8 take 0.51 s median (1.87 s in run 2), and their stalls are 30–50 % lower;
  - one display freezes DWM and Explorer for about 0.2 s per change, a batch of 8 for about 1 s; back-to-back
    changes queue up and freeze the desktop for seconds;
  - nothing stalls over 250 ms from 1 s after a change;
  - headless batches never brought the physical monitors back; one at a time, Windows did after 410 of 800 adds,
    and 26 adds took over 3 s including switching them off again;
  - Explorer crashed once (four times in run 2); turning off "taskbar on all displays" brings no consistent benefit.
- **Stall target decided** (2026-10-30, ADR 0006): no stall over 250 ms in steady state; during a display change,
  one freeze no worse than the run-3 baseline. From Phase 2 on, the host batches display changes, never chains them,
  and uses a stable ID per client monitor.
- **Not verified** (listed in `docs/phase-1-summary.md`): `SET_RENDER_ADAPTER`, re-claim by ID within the grace
  period, 119.88 Hz on real hardware, Windows 10, and recovery from a GPU reset.

## Phase 0 acceptance

| Criterion | Result |
|---|---|
| Everything builds unchanged in CI | ✅ host (Windows, MSYS2 UCRT64, incl. upstream tests), common-c (Linux/macOS/Windows), Qt client (Windows x64, macOS), Android, iOS (unsigned; deployment target raised to 12.0 on the command line for Xcode 26). Plus Tessera's own native components (MSVC and MinGW, 31/31 tests) and the VDD. |
| `recover.exe` restores a deliberately changed display topology | ✅ `tools/tests/topology-roundtrip.ps1` on the dev PC, 4/4 scenarios restored exactly in < 1 s each: move a display, swap the primary, change refresh rate (60 → 50 Hz), turn a display off. |

## Phase 0 deliverables
- ADRs 0001 (base fork), 0002 (monorepo), 0003 (own CCD layer), 0004 (dev environment).
- Public repo https://github.com/Stoicalboar7852/tessera: 5 upstream subtrees with full history, root
  `.gitmodules` generated by `tools/repo/sync-submodules.py`.
- CI: `ci-host-windows`, `ci-common-c`, `ci-client-qt`, `ci-client-android`, `ci-client-ios`, `ci-native`, `ci-driver`.
- `host-helpers/common`: write-ahead state journal (atomic replace, backup fallback, checksums, cross-process
  lock), CCD topology snapshot/restore with stable IDs and HDR, restore loop, `journaled_display_change`.
- `tools/recover` (`recover.exe`, `topoctl.exe`), `host-helpers/agent` (emergency hotkey Ctrl+Alt+Shift+F12).
- Service watchdog: `[fork]` hook in `host/tools/sunshinesvc.cpp` that restores before every (re)start of the host
  and when the service stops, through a `--tessera-restore` re-invocation in the console session.
- `drivers/vdd` skeleton (UMDF 2.25 + IddCx 1.4, zero monitors, version IOCTL), `tools/signing`, `tools/vdd-install`.
- Test VM tooling (`tools/vm`).

## Verified in the Secure Boot VM (Windows 11 Pro 26300, Hyper-V Gen 2, Secure Boot + vTPM, no test signing)
- **Driver** (`tools/vm/Test-VddInVm.ps1`: PASS):
  - With the dev certificate untrusted, the install is refused.
  - With it trusted, the VDD installs, starts (Status OK, no problem code), loads in WUDFHost and answers
    `IOCTL_TESSERA_VDD_GET_VERSION`. Code Integrity reports no blocks.
  - Removal and package purge are clean.
- **Service watchdog** (`tools/vm/Test-WatchdogInVm.ps1`: PASS), with the CI-built `sunshinesvc.exe`:
  - Journaled change, then service start: restored in 3.6 s.
  - Change while the host is gone: restored on the next relaunch attempt.
  - Change, then guest reboot: the journal survives, and the service restores at boot and clears the entry.
    Hyper-V resets the guest's resolution on reboot, so that restore was a no-op ("already-restored").

## Not yet verified (manual test procedures)
- **A layout change that survives a reboot, restored by the service at boot, on a physical PC.** Install the
  service elevated, run `topoctl change move-secondary`, reboot, log on, then `recover --list` must show 0 entries
  and `topoctl compare <saved>` must match.
- **Emergency hotkey:** start `tessera-agent.exe`, run `topoctl change move-secondary`, press Ctrl+Alt+Shift+F12, and
  check that the layout is back. It uses the same restore code as `recover.exe`, but nobody has pressed it in a test yet.
- **Local MSVC (EWDK) build** of the native components. CI builds them with MSVC; locally only the driver has been
  built with the EWDK.
- `vdd-install` reports the untrusted-publisher failure as win32 0x109, without the full SetupAPI/CERT_E code.
  Cosmetic; to look at in Phase 1.

## Decisions (2026-10-25)
- Product name **Tessera**.
- Public GitHub repo.
- Test VM: Windows 11 Pro (unactivated).
- Unattended work allowed (downloads, licenses, path mapping, real-monitor tests, VM admin account).
- D: is exFAT, so the dev environment uses `subst T:`, the EWDK, and repaired MSYS2 hard links (ADR 0004).

## Dev PC notes
- **Reinstalled 2026-10-26** as Windows 11 IoT Enterprise LTSC 2024 (26100); same hardware and BIOS settings. Ryzen 5
  7600X (AMD iGPU) + RTX 4060 Ti; Dell P2417H + H32S17. No third-party VDDs installed any more. C: now has ~420 GB
  free. The developer account is a local admin (UAC). The hypervisor is running (VBS). Redo checklist:
  `tools/setup/README.md`.
- Before the reinstall: Windows 11 Pro 26200 with MTT, Parsec and "Virtual Desktop" VDDs, Secure Boot off, ~17 GB free
  on C:.
- **Stability:** 7 unexpected restarts in 14 days, including 2 during this work (2026-10-26 06:12 and 06:29, no bugcheck
  recorded). Earlier crashes had bugchecks 0x1C7, 0x3B and 0x1E (2026-10-07/20/22/24), before this project started.
  These patterns usually point to memory instability or a faulty kernel driver. Worth checking before Phase 1, which
  stress-tests display drivers.
- "VirtualAudioDriver" (installed before this project) fails to load at boot (Kernel-PnP 219).
