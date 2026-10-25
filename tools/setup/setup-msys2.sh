#!/usr/bin/env bash
# Installs a portable MSYS2 into T:\toolchain\msys64 (T: = subst of the project folder; MSYS2 cannot live
# in a path with spaces) and brings it up to date. Package installation for the host build is separate.
set -euo pipefail
ver=20260927
dl=/t/toolchain/dl
mkdir -p "$dl"
cd "$dl"
f="msys2-base-x86_64-${ver}.sfx.exe"
[ -f "$f" ] || curl -sSL --retry 5 -o "$f" "https://repo.msys2.org/distrib/x86_64/$f"
# repo.msys2.org only publishes GPG .sig files; the GitHub release carries SHA-256 sums.
tag="${ver:0:4}-${ver:4:2}-${ver:6:2}"
curl -sSL -o "$f.sha256" "https://github.com/msys2/msys2-installer/releases/download/$tag/$f.sha256"
exp=$(awk '{print $1}' "$f.sha256"); act=$(sha256sum "$f" | awk '{print $1}')
[ "$exp" = "$act" ] || { echo "checksum mismatch: $exp vs $act"; exit 1; }
echo "checksum OK"
if [ ! -x /t/toolchain/msys64/usr/bin/bash.exe ]; then
  ./"$f" -y -o'T:\toolchain' >/dev/null
fi
M=/t/toolchain/msys64/usr/bin
# First login creates the home dir and keyring; then a full update (twice: core first, then the rest).
MSYSTEM=UCRT64 CHERE_INVOKING=1 "$M/bash.exe" -lc 'true'
MSYSTEM=UCRT64 "$M/bash.exe" -lc 'pacman -Syuu --noconfirm' || true
MSYSTEM=UCRT64 "$M/bash.exe" -lc 'pacman -Syuu --noconfirm'
echo "MSYS2 ready"
