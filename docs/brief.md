# Master Prompt: Multi-Display Game-Streaming Fork (Sunshine/Moonlight family)

> How to use this: paste everything below the line into Claude as the project brief. It is written to be
> worked **one phase at a time**. Start each new session with "Continue with Phase N", and keep
> `docs/STATUS.md` and the ADRs updated so each session picks up where the last one stopped.

---

## 1. Role

You are a senior systems engineer and software architect specialising in:
- Windows internals: the display stack (WDDM, IddCx indirect display drivers, CCD / `SetDisplayConfig`), DXGI/D3D11 capture, UMDF driver development, Win32 window management, services and sessions.
- Low-latency media: hardware video encoding (NVENC, AMF, QuickSync via FFmpeg), RTP/UDP transport, Opus audio.
- The Moonlight GameStream protocol as implemented by **Sunshine** (host, C++), **moonlight-common-c** (shared client protocol library, C), **moonlight-qt** (Windows/macOS, C++/Qt 6), **moonlight-android** (Android / Android TV) and **moonlight-ios** (iOS/iPadOS).
- Virtual-device stacks: ViGEmBus (virtual gamepads), USB/IP (usbip-win2), virtual audio endpoints, the Windows 11 virtual camera API.

You write production-quality, defensive code. You are honest about platform limits: when a requirement cannot be done cleanly on a platform, say so, propose the closest workable alternative, and record the decision. Never quietly drop a requirement.

## 2. Project goal

Fork the Sunshine (host) and Moonlight (client) stack and add:
1. **Multi-monitor streaming**: a client with N monitors receives N separate host displays, each shown on its own client monitor.
2. **Robust virtual displays on the host**: any number of virtual displays, at any resolution and refresh rate, with or without physical monitors attached. Adding displays must not freeze the system, as current virtual display drivers (VDDs) do when more than one display is added.
3. **Physical monitor blanking**: an option (next to "mute host audio") that turns off every physical host monitor the session is not using. The signal to the monitor must stop (the monitor goes to standby); a black image does not count.
4. **Multi-client and local-user arbitration**: extra clients, or a client connecting while someone is using the host in person, join as extra extended monitors on the same desktop.
5. **"Send to client display" window routing**: move any window to a chosen client display and make it fullscreen there, from the window title-bar menu and the taskbar icon menu.
6. **Game Mode**: a console-like mode that replaces the "Steam Big Picture" app. The Windows desktop is put into a minimal state ("hibernated") so games get as much of the machine as possible. Steam Big Picture is the shell, and non-Steam apps launch fullscreen and are switched between from Big Picture, similar to the Steam Deck.
7. **Peripherals**: full multi-controller support (including several controllers on one client for remote split-screen), microphone and camera passthrough from client to host, and optional USB / storage passthrough.

**Host platform:** Windows 10 22H2+ / Windows 11 only (primary target: Windows 11 Pro x64).
**Client platforms:** Windows, macOS (Apple Silicon), Android, Android TV / Google TV, iOS, iPadOS.

## 3. Starting point and repository layout

### 3.1 Base code (decide in Phase 0)
Evaluate these and record the decision in `docs/adr/0001-base-fork.md`:
- Upstream **Sunshine** (LizardByte) and the upstream Moonlight clients.
- **Apollo** (a Sunshine fork with an integrated virtual display driver, SudoVDA, and per-client virtual displays) and its Android client **Artemis**.
- Any other actively maintained forks you find.

Criteria: how far it already goes toward virtual-display lifecycle management, code health, recent activity, license (GPLv3 throughout, so all derived code stays GPLv3), and how easily changes can be rebased onto upstream. **Default unless research shows otherwise:** host based on Apollo; clients based on upstream moonlight-common-c, moonlight-qt, moonlight-android and moonlight-ios.

### 3.2 Monorepo layout
```
/host/                 Sunshine-derived host (C++20, CMake)
/host-helpers/         Companion processes in the user session: window router, game-mode helper, tray UI
/drivers/vdd/          Indirect display driver (UMDF 2 + IddCx, C++)
/drivers/thirdparty/   Pinned installers for ViGEmBus, usbip-win2, virtual audio driver
/common-c/             moonlight-common-c fork (shared protocol for all clients)
/clients/qt/           moonlight-qt fork (Windows, macOS)
/clients/android/      moonlight-android fork (Android + Android TV)
/clients/ios/          moonlight-ios fork (iOS + iPadOS)
/protocol/             Versioned protocol-extension spec + capability matrix
/docs/                 ADRs, STATUS.md, platform feature matrix, perf results, test plans
/tools/                Driver install/uninstall, test-signing setup, recovery tool
```
Keep upstream code in its original structure so upstream changes can still be merged. Put new features in new files and modules where possible, and keep edits to upstream files small.

## 4. Technical constraints (apply to every phase)

### 4.1 Languages and toolchains
| Component | Language / framework | Build |
|---|---|---|
| Host | C++20, matching Sunshine's existing style | CMake + MSYS2/UCRT64 (as upstream) |
| Host helpers | C++20, Win32 + C++/WinRT | CMake / MSVC |
| VDD driver | C++17, UMDF 2.x, IddCx (gate newer IddCx features behind version checks) | MSVC + WDK |
| common-c | C11 | CMake |
| Qt client | C++, Qt 6 | as upstream |
| Android client | Kotlin for new code; existing Java kept | Gradle |
| iOS/iPadOS client | Swift for new code; existing Obj-C kept | Xcode |

### 4.2 Error handling and robustness
- Check every Win32 return value, `HRESULT` and `NTSTATUS`. Log failures with the function name, the error code and its formatted message, and context (display ID, client ID, etc.).
- Use RAII for every handle and COM object (`wil`, `winrt::com_ptr`, custom deleters).
- Exceptions never cross C ABI, driver or thread boundaries. The driver uses no exceptions; it reports through `NTSTATUS` and TraceLogging/WPP.
- **Every change to host system state must be reversible and journaled.** Before changing display topology, the shell process, suspended processes, audio defaults, power plan or registry values, write the previous state to `%ProgramData%\<product>\state-journal.json`, atomically (temp file + replace). A watchdog restores from the journal when: the session ends, the host process crashes (the Windows service supervises it), the machine reboots (restore on service start), or the user presses an emergency hotkey (default `Ctrl+Alt+Shift+F12`) or runs `/tools/recover.exe`.
- Never leave the host with zero visible outputs and no way back. If a topology change fails or leaves no visible output, revert automatically within 10 s. Provide a "safe mode" config flag that turns off all display manipulation.
- Network: validate every length and field from the wire, bound every allocation, and treat clients as untrusted until paired.
- Graceful degradation: a missing feature on either side (old client, old host, unsupported platform) must never break the basic single-stream session.

### 4.3 Protocol compatibility
- Stay fully backward compatible with stock Moonlight clients and stock Sunshine hosts. Negotiate new features through capability flags in `/serverinfo` and the RTSP exchange, and document each flag in `/protocol/extensions.md` with a version number.
- All new channels (extra video streams, mic, camera, USB/IP, storage) are encrypted with the session's negotiated keys (AES-GCM, as Sunshine already does for control and input).
- Each extension has a per-paired-client permission toggle in the host web UI (for example, "this client may use USB passthrough").

### 4.4 Security and system integrity
- Drivers are installed only by the installer, with admin consent, never at runtime.
- Development: document test-signing in `/tools`. Release: document Microsoft attestation signing. Nothing may require turning off Secure Boot, HVCI or other OS security features.
- Use only documented, supported Windows APIs for interacting with other applications' windows. Do not modify or load code into other processes.

### 4.5 Performance targets (record results in `docs/perf.md`)
- Each extra display adds ≤ 2 ms of latency compared with a single stream, on LAN at 1080p60.
- Adding or removing a virtual display completes in < 3 s, with no system-wide stall > 250 ms and no hang. Test with 1, 2, 4 and 8 displays.
- Query the encoder session limit on each GPU vendor. When it is hit, show the user a clear message; never fail silently.

## 5. Known platform realities (design around these up front)

1. **Moonlight is one video stream per session.** Multi-monitor needs a protocol extension (several video streams in one session) and one capture → encode pipeline per display.
2. **Windows has one cursor and one keyboard focus per desktop.** Several clients on one desktop share them, so an explicit input policy is needed (F4).
3. **"Monitor off, no signal"** = deactivate that display path through the CCD API (`SetDisplayConfig`). DPMS (`SC_MONITORPOWER`) can't be used because it turns off all monitors at once.
4. **Shell Launcher is Enterprise/Education only.** On Pro, the per-user Winlogon `Shell` value works but only takes effect at logon.
5. **XInput supports only 4 controllers.** For more than 4 players, use DualShock 4 / DualSense emulation (ViGEmBus) or mixed controller types. ViGEmBus is archived upstream but still works; put it behind an `IVirtualGamepadBackend` interface so it can be replaced later.
6. **Title-bar and taskbar context menus belong to other processes / the shell**, and the Windows 11 taskbar has no public extension API for them. See F5.
7. **Windows 11 has a user-mode virtual camera API** (`MFCreateVirtualCamera`), so no kernel camera driver is needed. On Windows 10, camera passthrough is unsupported, and the UI says so.
8. **A virtual microphone needs a virtual audio driver.** Evaluate maintained, signed, open-source options and record the choice in an ADR; do not write a kernel audio driver from scratch.
9. **USB/IP:** usbip-win2 handles the Windows host (attach) side. Exporting devices from clients varies by platform: Android via the USB Host API in user space; Windows via libusb/WinUSB, which means rebinding the device's driver; macOS is limited, and kernel-claimed devices such as mass storage can't be exported; iOS can't do it at all.
10. **iOS/iPadOS:** no USB passthrough, single display on iPhone. iPad can use an external display (Stage Manager / `UIScene`). Android can use secondary displays through `Presentation` / `DisplayManager`.

## 6. Feature specifications

### F1. Virtual display driver (VDD)
- IddCx driver supporting **dynamic add/remove of N monitors at runtime** through an IOCTL control interface used by the host service. No reboot and no device restart.
- For each monitor, the host sets: resolution, refresh rate (including fractional rates such as 59.94 and 119.88), HDR on/off, and a stable ID, so Windows remembers its position and DPI per client.
- Each monitor gets a generated EDID/mode list containing the requested modes plus a small safe set. No huge mode lists.
- Set the render adapter explicitly to the GPU that does the encoding, so frames aren't copied between GPUs.
- **Fix the multi-monitor freeze.** In Phase 1, first reproduce it with the existing VDDs (SudoVDA, MikeTheTech Virtual-Display-Driver, parsec-vdd) and find the root cause before designing a fix. Investigate: swap-chain threads that block or spin without waiting, missing MMCSS registration, swap chains on the wrong adapter, oversized mode lists, device re-enumeration storms, and HDR negotiation. Write the findings in `docs/vdd-freeze-analysis.md`.
- Optional, behind a flag: capture straight from the driver's swap chain into a shared texture, as an alternative to Desktop Duplication / Windows.Graphics.Capture.
- Must work with **zero physical monitors** attached.

### F2. Multi-monitor streaming
- On connect, the client reports its monitors: count, resolution, refresh rate, HDR support, DPI, and left-to-right arrangement.
- The client UI lets the user stream: one display (as today), all client monitors, or a chosen subset.
- The host creates one virtual display per requested client monitor (or uses existing physical displays if the user picks that). It arranges them in the Windows topology to match the client's layout and runs one capture → encode → RTP pipeline per display.
- All streams share one session, one control channel and one input channel. Mouse coordinates are absolute and given per stream, and the host maps them onto the virtual desktop. Moving the mouse past a stream window's edge on the client carries it onto the neighbouring display.
- One audio stream per session.
- The client's total bandwidth limit is divided across streams, weighted by pixel rate. Frame pacing and FEC stay per stream.
- **Qt client:** one borderless fullscreen window per client monitor. **Android:** primary screen plus a `Presentation` on secondary displays. **iPadOS:** external display when connected. **iPhone / Android TV:** single display only; the option is hidden.

### F3. Physical monitor blanking
- Host option (global default, overridable per client): "Turn off host monitors not used by this session."
- Take a snapshot of the current CCD topology and journal it, then deactivate every physical path the session isn't using.
- On disconnect, crash, reboot or emergency hotkey, restore the exact previous topology: primary display, positions, rotation, refresh rate and HDR state.
- Never apply blanking while a local user is active (F4). Refuse, and log the reason, if blanking would leave no active output.

### F4. Session arbitration and multi-client
Implement an explicit host state machine (`SessionArbiter`) with states **Idle**, **LocalUserActive**, **DesktopClientActive(n)** and **GameModeActive**. Unit-test every transition.
- **LocalUserActive:** physical (non-injected) keyboard, mouse or controller input within a configurable window (default 5 min) while the console session is unlocked. Input generated by the streaming host itself must not count.
- **Game Mode is offered only in Idle.** In every other state the app list marks Game Mode as disabled with a reason, and the client shows it greyed out with that reason ("Host in use locally", "Desktop session active").
- **Joining as extra monitors:** a client that connects in Desktop mode during LocalUserActive or DesktopClientActive does not take over the existing displays. The host creates new virtual displays for it (one per requested client monitor) and **extends** the desktop onto them. Default placement: to the right of the existing layout; configurable.
- Clients that join this way never trigger blanking and don't take over the default audio device unless the host owner allows it.
- **Input policy** (configurable): *shared* (default; all clients drive one desktop, last input wins) or *confined* (while a joining client has focus, its mouse is limited to its own displays). Keyboard focus follows the last client that clicked.
- **Audio:** the primary client gets desktop audio. Per-app routing to a per-client audio endpoint is an experimental stretch goal.
- When a client disconnects, remove only that client's virtual displays, and move any windows on them back to a remaining display.
- Name each client display `<ClientName>-<n>`, n counting from 1, left to right in the client's physical arrangement. Use this name everywhere (UI, menus, logs).

### F5. "Send to client display" window routing
- A helper process in the interactive user session (`window-router.exe`, launched by the service) lists the current displays by their F4 names, plus "Local displays".
- Sending a window to a display moves it there and makes it **borderless fullscreen**. The original style and position are saved so "Restore window" puts it back.
- **Entry points requested:** the title-bar right-click menu and the taskbar icon right-click menu. Implement these only with documented, supported Windows mechanisms. If research in Phase 6 shows a supported way to add entries to those menus, use it. If not, say so in an ADR and ship these supported alternatives, which must exist either way:
  - A tray icon menu: "Send active window to ▸ …".
  - A configurable global hotkey that opens a small picker next to the cursor, applied to the window under the cursor or the foreground window.
  - A "Send to" control in the client's in-stream overlay menu.
- Window routing must be exposed through a small internal API (named pipe), so every entry point uses the same code path.

### F6. Game Mode
- **Entering:** offered only in Idle (F4). It journals the current state and then puts the desktop in a minimal state, using one of two configurable strategies:
  1. **Lightweight (default):** stop Explorer as the shell, suspend or close a user-configurable list of background apps, switch to a high-performance power plan, and launch Steam with Big Picture as the foreground fullscreen experience.
  2. **Dedicated user:** a separate local Windows account whose shell is Steam Big Picture (per-user Winlogon `Shell`). The host switches to it for Game Mode sessions. Document the trade-offs (separate Steam login state, a logon is required).
- **Exiting:** on disconnect or when the user exits from Big Picture, restore everything from the journal (Explorer restarted, processes resumed, power plan restored).
- **Non-Steam apps:** a host UI to pick apps (other launchers, Spotify, emulators, etc.). These are added to Steam as non-Steam shortcuts, written to `shortcuts.vdf` while Steam is closed (or using a tool such as Steam ROM Manager's approach), with launch wrappers that start each app borderless fullscreen on the Game Mode display.
- **Switching:** apps stay fullscreen, and the user switches between them through the Big Picture / Steam overlay, like the Steam Deck. Phase 7 must test how far Windows Big Picture supports switching between several running non-Steam apps. Where it falls short, provide a controller-friendly fallback switcher (Guide-button chord) that brings the chosen app to the front and keeps it fullscreen.
- Game Mode uses one stream (the client's primary display) unless the user enables multi-display.

### F7. Controllers
- Keep full Sunshine multi-controller support (rumble, motion, touchpad, adaptive triggers where supported).
- **Several controllers per client, several clients per host:** the host assigns global player slots across all clients and shows them in the web UI and client overlay. Up to 16 virtual pads; beyond 4, default to DS4/DualSense emulation (XInput limit).
- Hotplug on the client maps to hotplug on the host. Stable slot reassignment when a controller reconnects.
- Supported on every client platform (iOS/iPadOS via GameController framework; Android TV included).

### F8. Microphone and camera passthrough
- **Mic:** client captures audio → Opus → encrypted channel → host decodes → virtual microphone endpoint. Push-to-talk and mute toggle in the client. All client platforms.
- **Camera:** client captures camera → H.264/HEVC (hardware) → host decodes → Windows 11 virtual camera. Selectable camera and resolution on the client. Windows, macOS, Android, iOS/iPadOS; hidden on Android TV unless a camera is present.

### F9. USB and storage passthrough (optional, off by default)
- **Storage redirection (recommended path for drives):** the client exposes chosen folders or drives at file level over an encrypted channel, and the host mounts them as a drive letter (similar to RDP drive redirection). Far more tolerant of latency than block-level USB. Windows, macOS and Android clients; iOS/iPadOS through the Files provider APIs where possible.
- **Raw USB/IP:** for devices that need it. The host uses usbip-win2; the client exports devices where the platform allows (see §5.9). A LAN-only warning is shown when latency is high.
- Both need per-client permission (§4.3) and explicit user confirmation on the client for each device.

## 7. Platform feature matrix (keep updated in `docs/platform-matrix.md`)

| Feature | Win | macOS | Android | Android TV | iPadOS | iOS |
|---|---|---|---|---|---|---|
| Multi-monitor | ✅ | ✅ | ✅ (secondary display) | ❌ | ✅ (external display) | ❌ |
| Controllers (multi) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Mic | ✅ | ✅ | ✅ | ⚠️ remote mic | ✅ | ✅ |
| Camera | ✅ | ✅ | ✅ | ⚠️ if present | ✅ | ✅ |
| Storage redirection | ✅ | ✅ | ✅ | ⚠️ | ⚠️ | ⚠️ |
| Raw USB/IP | ✅ | ⚠️ limited | ⚠️ USB host only | ❌ | ❌ | ❌ |
| Window routing UI | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

## 8. Implementation plan

Work in this order. Each phase ends with: code that builds, tests passing, the acceptance criteria shown, `docs/STATUS.md` updated, and a short summary of what's done and what's next. **Stop after each phase and wait for confirmation before starting the next.**

- **Phase 0 – Research and setup.** Fork evaluation (ADR 0001), monorepo scaffold, CI builds for host and every client, test-signing setup script, the state journal + watchdog + recovery tool skeleton. *Acceptance:* everything builds unchanged in CI; `recover.exe` restores a deliberately changed display topology.
- **Phase 1 – VDD.** Reproduce and analyse the freeze, then build the driver with dynamic add/remove and per-monitor modes. *Acceptance:* 8 displays added and removed 100 times in a stress script, meeting the §4.5 targets, headless and with physical monitors.
- **Phase 2 – Protocol and host multi-stream.** Capability negotiation, multi-video-stream extension in common-c and host, per-display pipelines, bandwidth split. *Acceptance:* a test client receives 2–4 streams; stock Moonlight still works.
- **Phase 3 – Qt client multi-monitor.** Monitor discovery, layout mirroring, one window per monitor, edge-crossing mouse. *Acceptance:* a 2-monitor Windows client and a 2-monitor macOS client each stream 2 host displays.
- **Phase 4 – Monitor blanking.** CCD snapshot/deactivate/restore with journaling. *Acceptance:* physical monitors show "no signal" during the session and return exactly as before after disconnect, crash (kill the process) and reboot.
- **Phase 5 – Session arbiter and multi-client.** State machine, local-user detection, join-as-extra-monitors, input policies, per-client display naming. *Acceptance:* the scenarios in F4 pass as scripted integration tests and manual tests.
- **Phase 6 – Window routing.** Research supported menu integration (ADR), router API, tray/hotkey/overlay entry points. *Acceptance:* a window can be sent to `<Client>-2`, goes fullscreen there, and restores.
- **Phase 7 – Game Mode.** Both strategies, non-Steam shortcut management, fullscreen launch wrappers, switching test + fallback switcher, full restore. *Acceptance:* enter → play → switch apps → exit leaves the desktop exactly as it was; measured resource savings in `docs/perf.md`.
- **Phase 8 – Controllers.** Global slot allocator, >4 pads, multi-client split-screen. *Acceptance:* 2 clients × 2 controllers play a local split-screen game on the host.
- **Phase 9 – Mic and camera.** *Acceptance:* the client mic and camera show up in a host video-call app.
- **Phase 10 – Storage and USB.** Storage redirection first, then USB/IP. *Acceptance:* a client folder is mounted on the host; a USB flash drive is attached via USB/IP from a Windows or Android client.
- **Phase 11 – Mobile clients.** Port the common-c features to Android/Android TV and iOS/iPadOS according to the matrix. *Acceptance:* the matrix in §7 is verified on real devices.
- **Phase 12 – Hardening and release.** Fuzz the protocol parsers, run long soak tests, installer, driver signing documentation, user guide.

## 9. Working rules for you (Claude)

- Before writing code in a phase, give a short design for that phase (components, interfaces, files to touch, risks) and wait for approval.
- Ask when a requirement is ambiguous instead of guessing. Write decisions down as ADRs in `docs/adr/`.
- Prefer small, reviewable commits with clear messages, one concern per commit.
- Write unit tests for pure logic (arbiter, slot allocator, bandwidth split, protocol parsing, journal) and scripted integration tests for system behaviour (display add/remove, topology restore).
- Never claim something works without running it. When you can't test something (for example, it needs real hardware or a specific device), say so explicitly and give a manual test procedure.
- Keep the platform matrix and STATUS.md accurate at all times.
