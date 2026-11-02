#!/bin/bash
# Collects a Mac client's Moonlight logs and what the Phase 3 tests need to read them (displays, hardware, network
# link) into tessera-client-logs-<time>.zip on the Desktop. Needs no administrator rights.
#
#   bash Collect-ClientLogs.sh [newest]    # newest: how many of the newest Moonlight-*.log files (default 5)

set -euo pipefail

newest="${1:-5}"
name="tessera-client-logs-$(date +%Y%m%d-%H%M)"
work="$(mktemp -d)/$name"
mkdir -p "$work"

# Moonlight logs to /tmp on macOS (app/path.cpp)
count=0
while IFS= read -r log; do
  cp "$log" "$work/"
  count=$((count + 1))
done < <(ls -t /tmp/Moonlight-*.log 2>/dev/null | head -n "$newest")
echo "logs: $count"

{
  echo "Collected: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "macOS: $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
  echo
  system_profiler SPHardwareDataType SPDisplaysDataType 2>/dev/null | grep -v -E 'Serial Number|Hardware UUID|Provisioning UDID'
  echo "Network (active interfaces):"
  for dev in $(ifconfig -l); do
    if ifconfig "$dev" 2>/dev/null | grep -q 'status: active'; then
      echo "  $dev: $(ifconfig "$dev" | awk '/media:/ { $1 = ""; print }')"
    fi
  done
  echo
  echo "Service order:"
  networksetup -listnetworkserviceorder 2>/dev/null | grep -E '^\([0-9]+\)' || true
} > "$work/system.txt"

zip="$HOME/Desktop/$name.zip"
(cd "$(dirname "$work")" && zip -qr "$zip" "$name")
rm -rf "$(dirname "$work")"
echo "written: $zip"
