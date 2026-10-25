# ADR 0004: Development environment on the dev PC

- **Status:** Accepted (2026-10-25)

## Context

The development PC has an almost full system drive (C:, about 17 GB free). Everything else lives on an external
USB hard drive (D:, about 740 GB free, **exFAT**), in `D:\Remote streaming` (the path contains a space).
The developer account isn't elevated by default, and the owner is often away while work runs.

exFAT has no hard links, symlinks, junctions, ACLs or 8.3 names. Mounting a VHDX would need admin rights.

## Decision

| Need | Solution |
|---|---|
| A path without spaces for MSYS2, the NDK and build tools | `subst T: "D:\Remote streaming"`, recreated by `tools/setup/env.ps1` (not persistent). The repo is `T:\src`. |
| MSVC + Windows SDK + WDK without admin rights or space on C: | **EWDK** (VS 2026 Build Tools 18.3, WDK 10.0.28000), unpacked to `T:\toolchain\ewdk` |
| MSYS2 UCRT64 (host build) | Portable MSYS2 in `T:\toolchain\msys64`. pacman cannot create hard links on exFAT, so `tools/setup/msys2-fix-hardlinks.py` re-extracts the affected files from the verified package cache as copies. |
| git on exFAT | `safe.directory` entries for `D:/Remote streaming/*` and `T:/*` (exFAT records no owner), plus `gc.auto=0` and `maintenance.auto=false` (background repacks fail to rename packs on exFAT). |
| Caches (npm, Gradle, pip, NuGet, CMake FetchContent) | Redirected to `T:\cache` by `tools/setup/env.ps1` |
| Tests that need admin (driver install, service install) and Secure Boot | Hyper-V Gen 2 VM (Secure Boot on) stored on D:. Hyper-V VHDX files work on exFAT. |

The one-time elevated steps are in `tools/setup/admin-once.ps1` (Hyper-V, Hyper-V Administrators membership,
trusting the development code-signing certificate). The owner ran it on 2026-10-25.

## Consequences

- Builds on a USB HDD are slower than on an SSD. CI on GitHub-hosted runners is the reference build.
- The local MSVC toolset (v14.50 from EWDK) is newer than the VS 2022 toolset on `windows-2022`/`windows-2025`
  runners. Code must build with both.
- Development machines with NTFS drives don't need the subst, hard-link or safe.directory workarounds; the
  scripts detect this and skip them.
