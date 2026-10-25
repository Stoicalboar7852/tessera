# Tessera

Multi-display game streaming for Windows hosts, built on the Sunshine / Moonlight family.

Planned features:
- A client with N monitors receives N separate host displays.
- Robust virtual displays on the host.
- Physical-monitor blanking.
- Multi-client and local-user arbitration.
- "Send to client display" window routing.
- A console-like Game Mode with Steam Big Picture as the shell.
- Multi-controller, microphone, camera and USB/storage passthrough.

> **Status:** early development (Phase 0: research and setup). Nothing here is usable yet.
> See [docs/STATUS.md](docs/STATUS.md).

## Repository layout

| Path | What | Upstream |
|---|---|---|
| `host/` | Sunshine-derived host (C++20, CMake, MSYS2 UCRT64) | [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) |
| `host-helpers/` | Companion processes in the user session (state journal, agent, window router, game-mode helper) | new |
| `drivers/vdd/` | Indirect display driver (UMDF 2 + IddCx) | new |
| `drivers/thirdparty/` | Pinned third-party driver installers | — |
| `common-c/` | moonlight-common-c fork | [moonlight-stream/moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) |
| `clients/qt/` | moonlight-qt fork (Windows, macOS) | [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) |
| `clients/android/` | moonlight-android fork (Android, Android TV) | [moonlight-stream/moonlight-android](https://github.com/moonlight-stream/moonlight-android) |
| `clients/ios/` | moonlight-ios fork (iOS, iPadOS) | [moonlight-stream/moonlight-ios](https://github.com/moonlight-stream/moonlight-ios) |
| `protocol/` | Versioned protocol-extension spec and capability matrix | new |
| `docs/` | ADRs, status, platform matrix, perf results, test plans | new |
| `tools/` | Setup, signing, driver install, recovery tool | new |

Upstream code is imported with `git subtree`, so upstream changes can still be merged
(see [ADR 0002](docs/adr/0002-monorepo.md)). New features live in new files wherever possible.

## Getting the source

```bash
git clone https://github.com/Stoicalboar7852/tessera.git
cd tessera
git submodule update --init --recursive
```

## License

GPL-3.0, like the upstream projects; see [LICENSE](LICENSE). The virtual display driver is a separate program and
may carry its own license; this will be decided in the Phase 1 ADR. Third-party components keep their own licenses.

Tessera is not affiliated with LizardByte, the Moonlight project, or NVIDIA.
