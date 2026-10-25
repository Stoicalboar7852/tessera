# Project status: Tessera

_Last updated: 2026-10-25_

| Phase | State | Notes |
|---|---|---|
| 0 – Research and setup | **In progress** | Design approved (`docs/design/phase-0.md`). ADR 0001 accepted: host = upstream Sunshine. Waiting for the Hyper-V reboot, then toolchain, repo, CI, journal and recover.exe. |
| 1–12 | Not started | |

## Decisions (2026-10-25)
- Product name: **Tessera** (`%ProgramData%\Tessera`, repo `tessera`).
- GitHub: public repo under the owner's account.
- Test VM: Windows 11 Pro (unactivated), Hyper-V Gen 2, Secure Boot on.
- The owner allowed unattended work: downloads, accepting tool licenses, the `D:\rs` junction (the path has
  a space), topology tests on the real monitors, and a test admin account inside the VM.

## Known facts about the dev PC
Windows 11 Pro 26200; AMD iGPU + RTX 4060 Ti; 2 physical monitors; MTT, Parsec and "Virtual Desktop" VDDs
installed; Secure Boot **off**; C: has only ~17 GB free, so all tools, caches and VMs go on D: (HDD).
