# ADR 0001: Base code for host and clients

- **Status:** Accepted (2026-10-25, approved by the owner)
- **Deciders:** project owner
- **Supersedes:** none

## Context

The brief (§3.1) asks for a base for the host (Sunshine family) and the clients (Moonlight family).
The default is an **Apollo**-based host with upstream clients, *unless research shows otherwise*.
The criteria are: how far the base already goes toward virtual-display lifecycle management, code
health, recent activity, license, and how easily our changes can be rebased onto upstream.

Data below was collected on 2026-10-25 from the GitHub API and the project READMEs and release notes.
"Commits" counts commits on the default branch.

## Candidates

### Host

| Repository | Base | Commits, last 6 mo / 30 days | Latest release | License | Virtual display |
|---|---|---|---|---|---|
| **LizardByte/Sunshine** | upstream | 547 / 124 | v2026.1018.5342 (2026-10-18) | GPL-3.0 | None built in. The `dd_*` options reconfigure an *existing* display (resolution, refresh, HDR, primary, "only display", revert delay). No per-client virtual displays. |
| **ClassicOldSong/Apollo** | Sunshine fork | 4 / 0 | v0.4.7-alpha.1 (2025-08-12) | GPL-3.0 | SudoVDA. One virtual display per stream, created at stream start and removed when the app quits, with a fixed identity per client so Windows remembers its settings. HDR. |
| **Nonary/Vibepollo** | Apollo fork | 1812 / 844 | daily nightlies | GPL-3.0 | Bundled VDD (not named in the README; SudoVDA kept as a fallback). Up to 8 clients with their own virtual screens, saved client display arrangements, topology restore after crash, shutdown or reboot (not after logout). |
| Nonary/vibeshine | Sunshine fork | 1648 / 431 | daily nightlies | GPL-3.0 | Sibling of Vibepollo. |
| **AlkaidLab/foundation-sunshine** | Sunshine-based (not a GitHub fork) | 419 / 61 | daily (v2026.1025.82836) | GPL-3.0 | ZakoDisplay (zako-vdd), HDR, "Zako Direct" capture straight from the virtual display. Also has mic passthrough and folder sharing. |

Other observations:
- **Apollo** is effectively dormant. Its last commit (2026-05-21) merged a security fix, and there are 347 open
  issues and PRs. Its README says the author is "going away" and that Apollo/Artemis "will no longer be
  compatible with OG Sunshine and OG Moonlight eventually". Apollo branched from Sunshine more than a year ago,
  and upstream has had about 550 commits in the last 6 months alone.
- **Vibepollo** has the most features, but its README says about 99% of the code is AI-generated, that it
  "differs substantially from upstream Sunshine", and that large upstream backports are not sustainable.
  It has one maintainer and about 800 commits a month.
- **Foundation Sunshine** is active and has many features. Its docs and community are Chinese-first, its
  advanced features need its own client forks (Moonlight V+ and others), and it doesn't say whether it tracks upstream.
- **Upstream Sunshine** now ships signed MSI installers (since v2026.516) and has replaced ViGEmBus
  with **libvirtualhid** as the primary Windows gamepad backend (v2026.906). ViGEmBus can still be selected
  (`gamepad_driver`, v2026.914). The libvirtualhid library is MIT, but its Windows UMDF driver is under the
  *LizardByte Source-Available License 1.0*, and according to the release notes full driver-backed gamepad
  support needs a separately installed driver and a license.
  Phase 8 has to take this into account (see Consequences).

### Clients

| Repository | Commits, 6 mo / 30 days | Latest release | License | Notes |
|---|---|---|---|---|
| moonlight-stream/moonlight-common-c | 29 / 14 | (no releases; consumed as submodule) | GPL-3.0 | Active. |
| moonlight-stream/moonlight-qt | 186 / 50 | v6.2.0 (2026-10-04) | GPL-3.0 | Active. |
| moonlight-stream/moonlight-android | 32 / 0 | v12.2 (2026-09-12) | GPL-3.0 | Active. |
| moonlight-stream/moonlight-ios | n/a (rate-limited) | last push 2026-09-26 | GPL-3.0 | Active. |
| ClassicOldSong/moonlight-android (Artemis, branch `moonlight-noir`) | 4 / 0 | v20.3.0-experimental.9 (2025-08-30) | GPL-3.0 | Dormant like Apollo. |

Upstream Moonlight now sends a client identifier to the host for client-specific session management.
This helps with F4's stable per-client display identity.

### Virtual display drivers (reference only; F1 needs our own driver)

| Driver | License | Activity | Notes |
|---|---|---|---|
| VirtualDrivers/Virtual-Display-Driver (MTT) | MIT (label) | last push 2026-09-19 | IddCx, descends from `ge9/IddSampleDriver`, which descends from Microsoft's sample. Signed by SignPath Foundation (checked on the dev PC). |
| SudoMaker/SudoVDA | README: "MIT and CC0 or Public Domain (for changes I made …)" | last push 2025-04-14 | Rewrite of an MTT fork. `maxMonitors`, `gpuName`, watchdog settings. |
| nomi-san/parsec-vdd | MIT (controller only) | last push 2026-05-15 | Wraps Parsec's proprietary, Microsoft-signed driver. |
| microsoft/Windows-driver-samples (IddSampleDriver) | **MS-PL** | active | MS-PL is a free license, but it is **incompatible with GPLv3**. |

## Decision (proposed)

1. **Host: upstream LizardByte/Sunshine**, *not* Apollo. This overrides the brief's default.
2. **Clients: upstream** moonlight-common-c, moonlight-qt, moonlight-android and moonlight-ios.
   This matches the default. Artemis is not used, because it is as dormant as Apollo.
3. Apollo, Vibepollo and Foundation Sunshine are **references**, not bases. Where it helps, we port specific GPLv3
   code by hand into *new* files and record the source repository and commit in the file header and in
   `docs/provenance.md`. Planned candidates:
   - Apollo: per-client virtual display lifecycle and stable display identity (F1/F4); per-client permission
     model (§4.3).
   - Vibepollo: approach to recovering the topology after a crash or reboot (Phase 0/4), and client display arrangement (F2).
   - Foundation Sunshine: direct capture from the virtual display (F1 optional) and mic passthrough (F8).

## Rationale

- **Activity and rebasing:** Upstream is the most active project, and our changes stay a small, mergeable
  diff on top of it. Starting from Apollo would mean one very large merge to catch up with upstream, which we
  would then have to maintain alone, because Apollo is dormant and its author says it will diverge.
- **Apollo's main advantage is largely replaced anyway.** F1 needs a new IddCx driver with IOCTL add/remove
  and a root-caused freeze fix. Apollo's SudoVDA integration would be thrown away, and the host-side lifecycle
  code we would keep is small and can be ported.
- **Vibepollo** would give the most features on day one, but its churn and its explicit non-tracking of
  upstream make it the hardest base to review, trust and keep current.
- **Foundation Sunshine** tracks upstream in an undocumented way and is tied to its own client ecosystem.
- **License:** every candidate is GPL-3.0, so this criterion doesn't decide anything.

## Consequences

- Phases 1–2 involve more host-side work, because we write the virtual display lifecycle ourselves instead of inheriting it.
- Merging upstream stays cheap (see ADR 0002 for the repository mechanics).
- **Gamepads (Phase 8):** `IVirtualGamepadBackend` keeps ViGEmBus as the default open backend and may
  wrap libvirtualhid. The Phase 8 ADR must check whether libvirtualhid's source-available driver and its licence
  requirement can be redistributed with a GPLv3 product.
- **Driver license (Phase 1):** The Microsoft IddCx sample is MS-PL, which can't be combined with GPLv3 code.
  The VDD is a separate program that talks to the host only through IOCTLs, so it can carry its own license
  inside the same installer (an "aggregate" under GPLv3 §5). The Phase 1 ADR must choose between (a) deriving
  from the sample and licensing the driver MS-PL, and (b) writing it from the IddCx documentation under
  MIT/GPLv3. MTT's "MIT" label sits on code that descends from the MS-PL sample, so we don't copy from it
  until its provenance has been checked.

## Open items

- Confirm that upstream Sunshine offers no optional virtual-display component in its installer. Its release
  notes mention none; one third-party guide claims there is one.
- Identify Vibepollo's bundled VDD and its license (reference only).
