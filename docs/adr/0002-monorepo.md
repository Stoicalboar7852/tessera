# ADR 0002: Monorepo with git subtrees

- **Status:** Accepted (2026-10-25)

## Context

The brief (§3.2) asks for one repository with the host, the clients and the shared protocol library side by side,
and with upstream code kept in its original structure so upstream changes can still be merged.

## Decision

1. Each upstream is imported with **`git subtree add`, full history, no squash** (`tools/repo/import-upstreams.sh`):

   | Path | Upstream | Imported at |
   |---|---|---|
   | `common-c/` | moonlight-stream/moonlight-common-c | `f900dd47` (the commit moonlight-qt v6.2.0 pins) |
   | `host/` | LizardByte/Sunshine | `v2026.1018.5342` |
   | `clients/qt/` | moonlight-stream/moonlight-qt | `v6.2.0` |
   | `clients/android/` | moonlight-stream/moonlight-android | `v12.2` |
   | `clients/ios/` | moonlight-stream/moonlight-ios | `v3.0.0` |

   Later updates use `git subtree pull --prefix=<dir> <url> <tag>`, which is an ordinary merge commit.

2. **Nested submodules.** The subtrees keep their upstream submodules as gitlinks, but git only reads the root
   `.gitmodules`. `tools/repo/sync-submodules.py` regenerates the root `.gitmodules` from the subtrees' own files,
   resolving relative URLs against each upstream. CI checks that it is up to date (`--check`). After a
   `subtree pull`, run the script and commit the result together with the merge.

3. **One copy of common-c (deferred to Phase 2).** In Phase 0, every client and the host still build against its
   own pinned upstream `moonlight-common-c` submodule, so CI proves the unchanged builds. When Phase 2 first changes
   the protocol code, each consumer gets a small, explicit build-file override pointing at `/common-c`: a CMake
   cache variable for the host and the common-c CI, a qmake variable for Qt, an ndk-build variable for Android, and
   a project reference for iOS. The nested submodule is then removed.
   The links-or-junctions approach from the Phase 0 design was dropped, because the development drive is exFAT
   (no junctions or symlinks; see ADR 0004).

4. **Marking our changes in upstream files.** Every change to an upstream file carries a `[fork] Tessera` comment,
   so it can be found during merges (`git grep "\[fork\]"`). New functionality goes into new files.

## Consequences

- The full upstream histories make the repository large: about 550 MB of packs. Clone with `--filter=blob:none`
  if that matters.
- Upstream security fixes arrive through `subtree pull` merges; conflicts are limited to the `[fork]` lines.
- `git subtree split` can still extract a component's history if a fix should go upstream.
