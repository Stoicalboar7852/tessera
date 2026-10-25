# ADR 0003: Own CCD snapshot/restore instead of libdisplaydevice

- **Status:** Accepted (2026-10-25)

## Context

Phase 0 needs to snapshot the display topology and restore it exactly: active paths, primary, positions,
rotation, exact refresh rate, resolution and HDR (brief §4.2, F3). The code is shared by the MinGW-built host
(service watchdog) and the MSVC-built helpers and tools.

Upstream Sunshine uses LizardByte's **libdisplaydevice** (MIT, pinned at `a2afc043`) for its `dd_*` options.
At that commit, its Windows API covers:
- enumerating devices with stable device IDs;
- the active topology as groups of device IDs (clones);
- display modes (resolution plus rational refresh rate);
- the primary display;
- HDR state;
- a persisted "revert" state for Sunshine's own flow.

## Decision

Tessera uses its own small CCD layer (`host-helpers/common`, `tessera/display_topology.h`) instead of libdisplaydevice:

- **Coverage:** libdisplaydevice doesn't restore desktop **positions** or **rotation**, and its persistence models
  Sunshine's "configure one display, then revert" flow, not a general snapshot of everything.
- **Toolchains:** libdisplaydevice is built and tested only with MinGW (its CI uses MSYS2), and it depends on Boost
  (`algorithm`, `scope`, `uuid`). Our MSVC-built tools would have to pull in Boost for no other reason.
- **Size:** the needed functionality is about 700 lines of plain Win32 code. It depends only on nlohmann_json and
  compiles with both toolchains.

The ideas carry over unchanged: stable identities instead of LUIDs, rational refresh rates, and verification after
applying.

## Consequences

- Sunshine's own `dd_*` options and Tessera's display management must not both change the topology at once. When
  Tessera manages displays (virtual displays, blanking), the host disables `dd_configuration_option` for that session.
  To be implemented in Phase 1/4.
- **Not captured:** per-monitor DPI scaling (no public API; Windows keeps it per monitor), SDR white level (no
  public setter) and QDC_VIRTUAL_MODE_AWARE desktop scaling. Recorded in `docs/platform-matrix.md` limitations.
- The HDR calls use the Windows 11 24H2 `GET_ADVANCED_COLOR_INFO_2` / `SET_HDR_STATE` requests when available and
  fall back to the older advanced-colour requests.
