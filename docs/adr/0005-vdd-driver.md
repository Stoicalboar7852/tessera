# ADR 0005: Own IddCx driver, written from the documentation, licensed GPLv3

- **Status:** Accepted (2026-10-27, with the Phase 1 design)

## Context

Tessera needs a virtual display driver that adds and removes monitors at runtime without freezing the system (brief
F1). `docs/vdd-freeze-analysis.md` measured the three existing drivers on the dev PC: all of them miss the targets,
for reasons that are part of their design.

| Driver | License | Why not use or fork it |
|---|---|---|
| Microsoft IddCx sample (`IddSampleDriver`) | MS-PL | MS-PL code can't be relicensed under GPLv3, and MS-PL files can't be mixed into GPLv3 files. The sample is also single-adapter and fixed-monitor. |
| SudoVDA | no license file in its repository; derived from the MS-PL sample | No clear grant of rights. Its add cost grows with the display count, and it freezes for tens of seconds at 8 displays. |
| MTT Virtual Display Driver | MIT (it contains MS-PL sample code) | Fixed monitor count; changing it restarts the adapter. Single-worker thrash in the released driver. Its control pipe is open to every local user. |
| parsec-vdd | proprietary (closed source) | Can't be modified or redistributed with Tessera. It refuses adds after a few cycles. |

## Decision

- Tessera writes its own UMDF 2 / IddCx driver in `drivers/vdd` from Microsoft's public IddCx and WDF
  documentation (the API reference and the "Indirect display driver model" pages).
- The source of the drivers above was read only to analyse the freeze. No code is copied from them or from the MS-PL
  sample, and none is translated line by line. Where Tessera solves the same problem, it does so differently (see
  `docs/phase-1-design.md`).
- The driver is licensed **GPLv3**, like the rest of the repository. Facts needed for interoperability (IddCx
  structure layouts and call order, EDID and CTA-861 formats, CVT reduced-blanking timing formulas) are not
  copyrightable expression and are implemented from the public specifications.
- The pure parts (mode lists, timings, EDID bytes) live in `host-helpers/common` as header-only C++17 without
  exceptions or the STL. The driver and the user-mode tools compile the same code, and it is unit-tested on its own.

## Consequences

- More code to write and test than a fork, but the design can follow the measured findings instead of working
  around a fork's structure.
- Release signing for a GPLv3 driver (Microsoft attestation signing) is documented in `docs/release-signing.md`.
  Tessera's own builds are signed by its release key. Anyone rebuilding the driver signs it with their own
  certificate.
- Contributors must not paste code from the MS-PL sample or the third-party drivers. Reviews check new driver code
  against this rule.
