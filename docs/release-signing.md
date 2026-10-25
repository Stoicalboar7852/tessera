# Driver and binary signing

Tessera never needs test-signing mode, Secure Boot off, or HVCI off (brief §4.4).

## Development (dev PC and test VM)

The VDD is a **UMDF** (user-mode) driver: it runs inside `WUDFHost.exe` and isn't subject to the kernel-mode rule
that drivers must carry a Microsoft signature. The driver package only needs a signature that the machine trusts.

1. Create a code-signing-only certificate with a non-exportable key in `CurrentUser\My`. Done on the dev PC on
   2026-10-25 (`CN=Remote Streaming Dev Driver Signing (local only)`).
2. Trust its public part once, elevated: `tools/setup/admin-once.ps1` adds it to `LocalMachine\Root` and
   `LocalMachine\TrustedPublisher`. `admin-undo.ps1` removes it.
3. Build the driver, then sign the package:
   ```powershell
   .\tools\setup\ewdk.ps1 'msbuild drivers\vdd\TesseraVdd.vcxproj /p:Configuration=Release /p:Platform=x64'
   .\tools\signing\Sign-DriverPackage.ps1 -BuildDir T:\build\vdd\x64\Release
   ```
   This runs InfVerif, signs the DLL, generates the catalog with Inf2Cat, signs the catalog and verifies both.
4. Install it elevated: `vdd-install install <package>\TesseraVdd.inf`.

**Status:** whether a locally trusted certificate is enough with Secure Boot **on** is being checked in the Hyper-V
test VM (Gen 2, Secure Boot and vTPM on). See `docs/STATUS.md` for the result.

## Release

Two options, both compatible with Secure Boot and HVCI:

| Option | What it takes | Notes |
|---|---|---|
| **Microsoft attestation signing** (Partner Center, Hardware Dev Center) | Partner Center account, EV code-signing certificate to register, and a CAB with the driver package submitted per release | Microsoft signs the package; valid on Windows 10/11 client. No HLK run needed. The standard route for drivers. |
| **SignPath Foundation** (free code signing for open-source projects) | An application to SignPath Foundation, plus CI integration | The MTT Virtual Display Driver and Foundation Sunshine ship UMDF drivers signed this way. Verify per release that it is enough for a UMDF driver package on current Windows. |

The host executables and installer (MSI) are signed the same way as upstream Sunshine's (Azure Artifact Signing in
upstream CI), or through SignPath. Decided in Phase 12.

Kernel-mode third-party drivers (ViGEmBus, usbip-win2, a virtual audio driver) are only redistributed as their
vendors' signed installers (`drivers/thirdparty/`).
