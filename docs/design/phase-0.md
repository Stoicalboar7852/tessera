# Phase 0 design: research and setup

- **Status:** Waiting for approval (2026-10-25). No code is written until this is approved.
- **Acceptance (from the brief):** everything builds unchanged in CI; `recover.exe` restores a deliberately
  changed display topology.

## 0. What I found on the dev PC

| Item | Finding | Why it matters |
|---|---|---|
| OS | Windows 11 Pro 10.0.26200 | Primary target. |
| GPUs | AMD Radeon iGPU (Ryzen 7000) + NVIDIA RTX 4060 Ti | Hybrid setup: a good test for "render adapter = encoding GPU" (F1) and for DXGI output mapping. |
| Physical monitors | Dell P2417H, H32S17 (both active) | Lets us test the real topology restore and, later, blanking. |
| Existing VDDs | MTT Virtual Display Driver (active, plus 4 stale monitors), Parsec Virtual Display Adapter, "Virtual Desktop Monitor" (device in **Error** state) | Useful for reproducing the freeze in Phase 1. SudoVDA is not installed. |
| Secure Boot | **Off** (`UEFISecureBootEnabled = 0`) | This PC can't show whether our driver loads with Secure Boot **on** (§4.4). See §3. |
| HVCI / test signing | Both off | |
| Toolchain | Only Git for Windows, Python 3.15, winget and Chocolatey. **No** Visual Studio/MSVC, WDK, CMake, MSYS2, Qt, JDK/Android SDK or `gh`. | Must be installed before anything can build here (§6). |
| Driver signatures | MTT VDD (a UMDF/IddCx driver) is signed **only by SignPath Foundation**, not by Microsoft. Parsec's driver is Microsoft-signed. | Suggests that UMDF drivers don't need a Microsoft signature the way kernel drivers do. Not yet verified with Secure Boot on. |

macOS and iOS builds can't run on this PC. They will only be built and checked in CI.

## 1. Repository and monorepo mechanics (ADR 0002)

- One git repo in `D:\Remote streaming`, pushed to GitHub (owner and visibility to be decided, see Questions).
- Each upstream is imported with `git subtree add --prefix=<dir> <remote> <tag>`, keeping its full history.
  Later upstream changes come in with `git subtree pull`, which is an ordinary merge. Planned pins:

  | Path | Upstream | Pin |
  |---|---|---|
  | `host/` | LizardByte/Sunshine | `v2026.1018.5342` |
  | `common-c/` | moonlight-stream/moonlight-common-c | the commit moonlight-qt v6.2.0 pins |
  | `clients/qt/` | moonlight-stream/moonlight-qt | `v6.2.0` |
  | `clients/android/` | moonlight-stream/moonlight-android | `v12.2` |
  | `clients/ios/` | moonlight-stream/moonlight-ios | latest release tag |

- **Nested submodules problem:** Every upstream repo has git submodules (Sunshine's `third-party/*`, and each
  client embeds `moonlight-common-c`). Git ignores a `.gitmodules` file that isn't at the repository root. Plan:
  - Every nested `moonlight-common-c` is **removed** and replaced by a link to our single `/common-c`.
    `tools/repo/bootstrap.ps1` creates the links after cloning: directory junctions on Windows, which need no admin
    rights or Developer Mode, and symlinks on macOS/Linux via `bootstrap.sh`. The link paths are git-ignored.
    Upstream build files stay unchanged. When upstream bumps that submodule, `subtree pull` raises a trivial
    modify/delete conflict. We keep the deletion and treat the conflict as a reminder to pull common-c too.
  - All other nested submodules are re-registered in the **root** `.gitmodules` at their nested paths, pinned to
    upstream's commits. `tools/repo/sync-submodules.py` regenerates them after each upstream pull.
- New code lives in new directories (`host-helpers/`, `drivers/vdd/`, `tools/`, `protocol/`) or in new files
  inside upstream trees. Changes to upstream files are kept minimal and tagged with a `// [fork]` comment so
  they are easy to find during merges.

## 2. CI (GitHub Actions)

Each component has its own workflow with a path filter, so a change in one component doesn't rebuild everything.
Phase 0 builds the **unchanged** upstream code, reusing each upstream's own build recipe.

| Workflow | Runner | Build |
|---|---|---|
| `host-windows` | windows-latest | MSYS2 UCRT64 (`msys2/setup-msys2`), CMake + Ninja, upstream unit tests |
| `common-c` | ubuntu, windows, macos | CMake |
| `client-qt` | windows-latest; macos (arm64) | MSVC + Qt 6 (upstream scripts); macOS unsigned `.dmg` |
| `client-android` | ubuntu-latest | JDK + Android SDK/NDK, `assembleDebug` |
| `client-ios` | macos (arm64) | `xcodebuild` for iOS and iPadOS, `CODE_SIGNING_ALLOWED=NO` |
| `helpers-tools` | windows-latest | MSVC + CMake for `recover.exe`, agent and unit tests; also compiles the shared journal code with MinGW (see §4.1) |
| `driver-vdd` | windows-latest | MSVC + WDK (NuGet package, so no WDK installer is needed in CI), InfVerif, signs with a throwaway CI certificate |

Upstream `.github/` folders inside subtrees are inert, because GitHub only reads the root one.
Signing for Apple, Android release keys and Windows release signing are not part of Phase 0.

## 3. Driver signing setup (`/tools/signing`)

**Constraint conflict:** `bcdedit /set testsigning on` only works with Secure Boot off, and §4.4 says nothing
may require turning Secure Boot off. So the plan **doesn't use test-signing mode**:

- UMDF drivers run in user mode inside `WUDFHost.exe` and aren't subject to the kernel's rule that drivers
  must carry a Microsoft signature. The SignPath-signed MTT driver on this PC supports this. The plan:
  - `New-DevCodeSigningCert.ps1` creates a self-signed certificate that can only sign code, with a
    non-exportable key in your user store.
  - `Install-DevCert.ps1` adds the public certificate to `LocalMachine\Root` and `TrustedPublisher`, and
    `Uninstall-DevCert.ps1` removes it. **You run these yourself, elevated.** Adding a trusted root is a
    security setting, so I won't run it for you. Anyone holding that key can sign code this PC trusts, which is
    why the key is non-exportable and the uninstall script exists.
  - `Sign-DriverPackage.ps1` runs Inf2Cat and signtool on the driver package.
- **Verifying this with Secure Boot on is required.** This PC has Secure Boot off, so it can't prove it.
  Proposal: a Hyper-V Generation 2 VM, where Secure Boot is on by default. IddCx works there without a GPU.
  If a locally trusted certificate turns out *not* to be enough there, the fallbacks are (a) SignPath
  Foundation's free open-source code signing (MTT VDD and Foundation Sunshine use it), or (b) attestation
  signing.
- `docs/release-signing.md` documents attestation signing through Partner Center: EV certificate, client OS only,
  no HLK needed. It also documents the SignPath option.
- **Minimal driver skeleton (`/drivers/vdd`):** a UMDF 2 IddCx driver that loads, initialises the adapter,
  reports 0 monitors and answers one `GET_VERSION` IOCTL. Plus `vdd-install.exe`, which creates the
  root-enumerated device node with SetupAPI (devcon can't be redistributed). This isn't the Phase 1 driver. It
  exists so that toolchain, signing and installation are proven in Phase 0, and Phase 1 can focus on the hard part.

## 4. State journal, watchdog and recovery tool

### 4.1 Toolchain split (key design constraint)
The host is built with **MinGW-GCC** (upstream Sunshine, MSYS2) and the helpers/driver tools with **MSVC**.
C++ objects can't cross between the two. So:
- The shared journal code is a **source-level** C++20 library (`host-helpers/common/statejournal/`) that is
  compiled into each binary separately. It uses only plain Win32, small custom RAII wrappers (no WIL in shared
  code, because WIL doesn't support MinGW well) and nlohmann/json, which Sunshine already depends on.
- Processes talk to each other only through named pipes that carry versioned JSON messages. No C++ ABI is shared.

### 4.2 Journal (`%ProgramData%\<Product>\state-journal.json`)
- **Write-ahead:** an entry is written as `pending` (with the *before* state) before any system change, marked
  `applied` afterwards, and removed after a successful restore.
- **Atomic:** write `*.tmp`, then `FlushFileBuffers`, then `ReplaceFileW`. The previous generation is kept as
  `*.bak`. When loading, a file that fails to parse or fails its checksum falls back to `.bak`. Each file carries a
  generation counter and a SHA-256 of its body.
- **Security:** the SYSTEM service *acts on* this file, so it must not be writable by ordinary users. The ACL is
  SYSTEM and Administrators full control, Users read. Every handler validates its entries; for example, the
  registry handler later only touches allowlisted keys. Size limit is 1 MiB, and entries are bounded.
- **Schema:** `{version, generation, entries:[{id, kind, owner, created, state, before, after}]}`.
- **Handlers:** an interface `IStateHandler { kind; capture; apply; restore; verify }`. Phase 0 implements
  only `display_topology`. Shell, processes, audio, power plan and registry handlers come in later phases.

### 4.3 Display topology handler
- Snapshot with `QueryDisplayConfig(QDC_ALL_PATHS)` and `DisplayConfigGetDeviceInfo`. The snapshot records
  positions, primary, rotation, scaling, refresh rate (as a rational number), resolution, HDR / advanced colour state and active paths.
- Adapter LUIDs change across reboots, so the journal stores **stable IDs** (monitor device path, adapter
  device instance path, target ID) and maps them back to current LUIDs on restore.
- Restore with `SetDisplayConfig(SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE |
  SDC_ALLOW_CHANGES | SDC_VIRTUAL_MODE_AWARE)`, then set HDR per target, then **verify** by re-reading and
  comparing.
- **Safety net (§4.2 of the brief):** after any apply, if no active output is visible within 10 s, the handler
  reverts automatically. A `safe_mode` config flag turns off all display changes.
- **Reuse check:** LizardByte's `libdisplaydevice` (MIT, used by Sunshine for its `dd_*` options) already does much
  of this. In Phase 0 I'll read its source and record the outcome in **ADR 0003**: reuse it if it also builds
  with MSVC and covers stable IDs and HDR, otherwise write our own handler.
- `SetDisplayConfig` must run in the **interactive** session. The service (session 0) runs restores through a
  process in the console session, the same way Sunshine already launches itself.

### 4.4 `recover.exe` (`/tools/recover`, MSVC)
- `recover --list | --dry-run | --restore [--kind display_topology] [--journal <path>]`.
- Works with the service stopped. Restoring the display topology needs no admin rights when run in your
  session. Later handlers for registry, services and the shell will need elevation and will say so.
- Exit codes and a readable log go to `%ProgramData%\<Product>\logs\recover.log`.

### 4.5 Watchdog and emergency hotkey (skeleton in Phase 0)
- **Service:** `host/` already has a service (`sunshinesvc`) that starts `sunshine.exe` in the console session.
  A small `[fork]` hook adds three triggers: on service start, restore if the journal is not empty (covers reboot);
  if the host exits abnormally, restore before relaunching (covers crash); on session logoff or disconnect, restore.
- **Hotkey:** `host-helpers/agent.exe` runs in your session, registers `Ctrl+Alt+Shift+F12` and asks the
  service over a named pipe to restore. It can't fire on the secure desktop (lock screen, UAC prompt). That is
  documented, and `recover.exe` covers that case.
- Phase 0 delivers: start-up restore and the hotkey path working end to end. Crash supervision is tested
  against a dummy "host" process that deliberately crashes. The real host only starts writing entries in Phase 1.

### 4.6 Tests
- **Unit (GoogleTest, runs in CI):** atomic write and recovery from a power cut (simulated by truncated or
  corrupt files), `.bak` fallback, checksum and generation checks, schema and size limits, topology
  serialisation round-trip using recorded `QueryDisplayConfig` fixtures from this PC, and LUID remapping.
- **Integration (real PC, not CI):** `tools/tests/topology-roundtrip.ps1` takes a snapshot, changes the topology
  (moves a monitor, changes the primary, changes a refresh rate, turns off the MTT virtual monitor), runs
  `recover.exe --restore` and checks the result field by field. **This changes your real display layout**, so it
  only runs when you start it, with the 10 s auto-revert active.

## 5. Docs created in Phase 0
`docs/STATUS.md`, `docs/platform-matrix.md` (§7 table, every cell marked "planned"), `docs/perf.md` (template
for §4.5), `docs/provenance.md`, `protocol/extensions.md` (versioning rules, no flags yet), and ADRs 0001–0003.

## 6. Toolchain to install on this PC
Installed via winget where possible, into default locations:
- Visual Studio **Build Tools** (C++ workload, Spectre libs, ATL) with the Windows SDK and **WDK**. I'll use the
  VS version the current WDK officially supports and check this at install time.
- MSYS2 (UCRT64), plus the packages upstream Sunshine lists.
- CMake, Ninja, GitHub CLI (`gh`). You do `gh auth login` yourself.
- Qt 6 (MSVC build) via `aqtinstall` into `C:\Qt`.
- Temurin JDK and Android command-line tools, SDK and NDK at the versions upstream Android pins.

Rough disk use: 30–45 GB.

## 7. Risks
1. **UMDF signing with Secure Boot on is unverified.** We check it early in the VM, and the fallbacks are listed in §3.
2. **Subtree plus nested submodules** is fiddly. It is handled by scripts and tested in CI on a fresh clone.
3. **MinGW/MSVC split:** shared code is compiled into each side separately, and processes communicate only
   through pipes. This is a lasting architectural rule.
4. **Topology tests touch your real displays.** They run only when you start them, with auto-revert.
5. **CI cost:** on a *private* repo, macOS runner minutes count 10x against the free quota. Public repos are free.
6. **Upstream gamepad licensing** (libvirtualhid). Deferred to the Phase 8 ADR, and doesn't block anything now.

## 8. Order of work once approved
1. Install the toolchain, `git init`, import the subtrees, bootstrap scripts. Local build of the host and Qt client on Windows.
2. CI workflows until everything is green, including the macOS/iOS/Android jobs that only run in CI.
3. Journal library and unit tests, display topology handler (plus ADR 0003), `recover.exe`.
4. Watchdog hook in the service, `agent.exe` hotkey.
5. Signing scripts, driver skeleton, `vdd-install.exe`. You install the dev cert; then we verify on this PC and in the Secure Boot VM.
6. Topology round-trip integration test on this PC (with you present), update STATUS.md, Phase 0 summary.

Each step is its own small commit (or a few).
