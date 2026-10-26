# Phase 0 summary: research and setup

_Completed 2026-10-26. Details are in `docs/STATUS.md`, ADRs 0001–0004 and `docs/design/phase-0.md`._

## Acceptance

| Criterion (brief §8) | Result | Evidence |
|---|---|---|
| Everything builds unchanged in CI | ✅ | GitHub Actions: `host (Windows)`, `common-c`, `client Qt (Windows, macOS)`, `client Android`, `client iOS`, `native components`, `driver (VDD)` |
| `recover.exe` restores a deliberately changed display topology | ✅ | `tools/tests/topology-roundtrip.ps1` on the dev PC, 4/4 scenarios restored exactly |
| Extra: driver signing works without disabling Secure Boot (brief §4.4) | ✅ | `tools/vm/Test-VddInVm.ps1` in a Secure Boot VM: an untrusted signature is refused; with the dev cert trusted, the VDD starts and answers its IOCTL |
| Extra: watchdog restores after service start, host crash and reboot | ✅ | `tools/vm/Test-WatchdogInVm.ps1` in the same VM. The reboot case was a no-op, because Hyper-V resets the guest resolution. |

Not verified, with manual procedures in `docs/STATUS.md`:
- a layout change that persists across a reboot on a physical PC;
- pressing the emergency hotkey;
- a local MSVC build of the native components (CI covers MSVC).

## What exists now

- **Base and repository:** upstream Sunshine and upstream Moonlight clients, imported as git subtrees with full
  history (ADR 0001, 0002). Public repo https://github.com/Stoicalboar7852/tessera.
- **CI** for every component. Upstream code is built with its own recipes.
- **State journal** (`host-helpers/common`): write-ahead entries (pending, then applied, then removed); atomic
  replace with a `.bak` generation; SHA-256 checksum; size and entry limits; cross-process lock; strict validation of
  untrusted input; a "restricted ACL" helper for the service. 21 unit tests.
- **Display topology** (ADR 0003): CCD snapshot/restore of active paths, primary, positions, rotation, scaling,
  exact rational refresh rates, full timing and HDR (24H2 API with fallback), keyed by stable IDs. Verify after apply;
  immediate revert if a change fails or leaves no active output. 10 unit tests.
- **Recovery paths:**
  - `recover.exe`: list / restore / dry-run / fallback to Windows' stored layout.
  - **Service watchdog:** a `[fork]` hook in `sunshinesvc.cpp` that restores before every (re)start of the host and
    when the service stops, through `--tessera-restore` in the console session.
  - **Emergency hotkey:** `tessera-agent.exe`, Ctrl+Alt+Shift+F12.
- **VDD skeleton** (`drivers/vdd`): UMDF 2.25 + IddCx 1.4. Loads, creates one adapter with zero monitors, and
  answers a version IOCTL. Runs in its own WUDF host group; control interface limited to SYSTEM and Administrators.
- **Signing and install:** `tools/signing/Sign-DriverPackage.ps1` (InfVerif, Inf2Cat, signtool with the dev
  certificate, no test-signing mode) and `tools/vdd-install` (SetupAPI root device create/update/remove/query).
- **Dev environment** (ADR 0004): exFAT workarounds (subst, MSYS2 hard-link repair, git safe.directory), the EWDK,
  and a Secure Boot test VM with tooling.

## Findings worth knowing

1. **Apollo is dormant**, so the host base is upstream Sunshine. Apollo, Vibepollo and Foundation Sunshine stay as
   references (ADR 0001).
2. **Upstream Sunshine replaced ViGEmBus** with libvirtualhid, whose Windows driver is source-available and needs a
   licence. This matters for Phase 8.
3. **Driver license:** the Microsoft IddCx sample is MS-PL, which is incompatible with GPLv3. Our driver code is
   written from the docs; the Phase 1 ADR fixes its license.
4. **The dev PC is unstable on its own:** 7 unexpected restarts in 14 days, and bugchecks 0x1C7, 0x3B and 0x1E before
   this project started. Phase 1 stress-tests display drivers, so this should be fixed first (memory test, EXPO/XMP
   off, driver updates), or results will be ambiguous.

## Not covered by Phase 0 (by design)

Multi-monitor protocol, real virtual monitors, blanking, arbitration, window routing, Game Mode and peripherals
(Phases 1–11). Release signing, installer and fuzzing (Phase 12).

## Next: Phase 1 (VDD)

Reproduce the multi-display freeze with MTT VDD, SudoVDA and parsec-vdd, and root-cause it
(`docs/vdd-freeze-analysis.md`). Then dynamic add/remove of N monitors over IOCTL, per-monitor modes and EDID, and
explicit render-adapter selection. Stress target: 8 displays × 100 add/remove cycles, headless and with physical
monitors. A design for Phase 1 comes first, for approval.
