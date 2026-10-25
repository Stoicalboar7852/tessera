#!/usr/bin/env bash
# One-time import of the upstream projects as git subtrees (full history, no squash).
# Later updates: git subtree pull --prefix=<dir> <remote> <tag>   (see docs/adr/0002-monorepo.md)
set -euo pipefail
cd "$(dirname "$0")/../.."

add() {  # add <prefix> <remote-name> <url> <ref>
  local prefix=$1 name=$2 url=$3 ref=$4
  if [ -d "$prefix" ] && [ -n "$(ls -A "$prefix" 2>/dev/null)" ]; then echo "skip $prefix (exists)"; return; fi
  git remote get-url "$name" >/dev/null 2>&1 || git remote add "$name" "$url"
  git fetch --no-tags "$name" "refs/tags/$ref:refs/tags/upstream/$name/$ref" 2>/dev/null \
    || git fetch --no-tags "$name" "$ref"
  local rev
  rev=$(git rev-parse -q --verify "refs/tags/upstream/$name/$ref^{commit}" || git rev-parse FETCH_HEAD)
  echo "== $prefix <- $url @ $ref ($rev)"
  git subtree add --prefix="$prefix" "$rev" -m "Import $prefix from $url at $ref

Upstream commit: $rev

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
}

# common-c is pinned to the commit moonlight-qt v6.2.0 uses, so all clients start from the same protocol code.
add common-c        upstream-common-c https://github.com/moonlight-stream/moonlight-common-c.git f900dd4767759c7b9d0e93bcea666b55c69ea62f
add host            upstream-host     https://github.com/LizardByte/Sunshine.git                  v2026.1018.5342
add clients/qt      upstream-qt       https://github.com/moonlight-stream/moonlight-qt.git        v6.2.0
add clients/android upstream-android  https://github.com/moonlight-stream/moonlight-android.git   v12.2
add clients/ios     upstream-ios      https://github.com/moonlight-stream/moonlight-ios.git       v3.0.0
echo "done"
