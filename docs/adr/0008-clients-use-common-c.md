# ADR 0008: The Qt client builds the monorepo's common-c

- **Status:** Accepted (2026-11-02, with `docs/phase-3-design.md`)

## Context

Each upstream Moonlight client builds moonlight-common-c from its own git submodule. In this monorepo the Qt client's
submodule is `clients/qt/moonlight-common-c/moonlight-common-c`. Phase 2 added multistream support to the monorepo's
`/common-c` (ADR 0007): the `Tessera*.c` files, about 80 marked lines in upstream files, and the CI tests. The Qt
client needs that code for Phase 3.

`/common-c` was imported from upstream commit f900dd47, the same commit the Qt client's submodule points at, so the
two differ only by the Tessera changes.

## Decision

The Qt client builds `/common-c` instead of its submodule. `clients/qt/moonlight-common-c/moonlight-common-c.pro`
points `COMMON_C_DIR` at `/common-c` and adds the Tessera sources and `/protocol/include`; the change is marked
`[fork] Tessera`. The submodule stays in `.gitmodules` for now but is not built. The Qt CI workflow also runs when
`common-c/**` or `protocol/**` change.

The Android and iOS clients move to `/common-c` when their phases need multistream. Once they have, the client
submodules are removed in one clean-up.

## Consequences

- One copy of common-c to maintain. Upstream common-c updates go into `/common-c` as subtree merges, and every
  client picks them up together.
- The Qt client can no longer take an upstream common-c bump on its own by moving its submodule; it gets one when
  `/common-c` is updated.
- A common-c change can now break the Qt build, so the Qt CI runs on common-c changes.
