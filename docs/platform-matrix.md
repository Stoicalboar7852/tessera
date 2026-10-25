# Platform feature matrix

Legend: ✅ planned (supported by design) · ⚠️ limited · ❌ not possible on the platform.
"Verified" stays empty until the feature has been tested on a real device (Phase 11 checks the whole matrix).

| Feature | Win | macOS | Android | Android TV | iPadOS | iOS | Verified |
|---|---|---|---|---|---|---|---|
| Multi-monitor | ✅ | ✅ | ✅ secondary display (`Presentation`) | ❌ | ✅ external display | ❌ | — |
| Controllers (multi) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Mic | ✅ | ✅ | ✅ | ⚠️ remote mic | ✅ | ✅ | — |
| Camera | ✅ | ✅ | ✅ | ⚠️ if present | ✅ | ✅ | — |
| Storage redirection | ✅ | ✅ | ✅ | ⚠️ | ⚠️ Files provider | ⚠️ Files provider | — |
| Raw USB/IP | ✅ | ⚠️ no kernel-claimed devices | ⚠️ USB host only | ❌ | ❌ | ❌ | — |
| Window routing UI | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — |

## Host (Windows) limitations known so far

| Area | Limitation | Source |
|---|---|---|
| Camera passthrough | Windows 11 only (`MFCreateVirtualCamera`); unsupported on Windows 10, and the UI says so | brief §5.7 |
| XInput | 4 pads max; beyond 4, DualShock 4 / DualSense emulation | brief §5.5 |
| Shell Launcher | Enterprise/Education only; Pro uses per-user Winlogon `Shell` | brief §5.4 |
| Topology restore | Per-monitor DPI, SDR white level and virtual-mode desktop scaling are not captured | ADR 0003 |
| Emergency hotkey | Can't fire on the secure desktop (lock screen, UAC prompt); `recover.exe` and the service watchdog cover it | Phase 0 design §4.5 |
