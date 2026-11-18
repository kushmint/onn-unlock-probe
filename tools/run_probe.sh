#!/usr/bin/env bash
# Push and run the onn-unlock-probe binary over adb.
#
# Usage:
#   tools/run_probe.sh [adb-serial]
#
# What it does:
#   1. Picks the right prebuilt binary for the device's ABI.
#   2. Pushes it to /data/local/tmp and runs it READ-ONLY (no args).
#      This step alone never writes anything.
#   3. Shows you the result and asks before doing anything else.
#   4. Only if you explicitly confirm does it run the reversible
#      --toggle-test, which flips the AVB lock bit and immediately
#      tries to flip it back, reporting whether the restore worked.
#
# This script NEVER reboots the device, at any point.
set -euo pipefail

SERIAL_ARG=()
if [[ $# -ge 1 ]]; then
  SERIAL_ARG=(-s "$1")
fi

adb "${SERIAL_ARG[@]}" wait-for-device
ABI=$(adb "${SERIAL_ARG[@]}" shell -n getprop ro.product.cpu.abi | tr -d '\r\n')
DEVICE=$(adb "${SERIAL_ARG[@]}" shell -n getprop ro.product.device | tr -d '\r\n')
MODEL=$(adb "${SERIAL_ARG[@]}" shell -n getprop ro.product.model | tr -d '\r\n')
echo "Target: $MODEL ($DEVICE), abi=$ABI"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$ABI" in
  arm64-v8a) LOCAL_BIN="$ROOT/bin/arm64-v8a/probe" ;;
  armeabi-v7a|armeabi) LOCAL_BIN="$ROOT/bin/armeabi-v7a/probe" ;;
  *)
    echo "Unrecognized ABI '$ABI'. This device isn't covered by the prebuilt binaries." >&2
    echo "Report this - it's a useful 'not compatible' result on its own." >&2
    exit 1
    ;;
esac

REMOTE=/data/local/tmp/onn-unlock-probe
adb "${SERIAL_ARG[@]}" push "$LOCAL_BIN" "$REMOTE" >/dev/null
adb "${SERIAL_ARG[@]}" shell -n chmod 755 "$REMOTE"

echo
echo "== Read-only check (no writes, zero risk) =="
adb "${SERIAL_ARG[@]}" shell -n "$REMOTE" || true
echo
echo "The line above ending in \"finished\" has the outcome. Common ones:"
echo "  already_unlocked_no_write_attempted        -> bit is already 0, nothing to test"
echo "  locked_read_only_rerun_with_toggle_test...  -> bit is 1 (locked); a toggle test is possible"
echo "  no_tee_device / tee_version_ioctl_failed    -> no OP-TEE on this device, not applicable"
echo "  ta_not_present_or_session_refused           -> same TA/UUID isn't present here"
echo

ANSWER=""
read -r -p "Run the REVERSIBLE toggle test now? This flips the lock bit to 0 then immediately back to 1, and reports whether the restore worked. Nothing reboots. [y/N] " ANSWER || true
if [[ "$ANSWER" =~ ^[Yy]$ ]]; then
  echo
  echo "== Toggle test (flip to 0, verify, flip back to 1, verify) =="
  adb "${SERIAL_ARG[@]}" shell -n "$REMOTE" --toggle-test || true
  echo
  echo "If the final outcome above starts with CRITICAL, the bit was NOT restored."
  echo "DO NOT REBOOT this device - re-run this script (read-only pass) to recheck"
  echo "the state before deciding what to do next."
else
  echo "Skipped. No writes were made."
fi

echo
echo "Cleaning up the pushed binary (this does not touch the lock state)."
adb "${SERIAL_ARG[@]}" shell -n rm -f "$REMOTE"
