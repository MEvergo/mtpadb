#!/usr/bin/env bash
set -euo pipefail

: "${MTPADB_BIN:?set MTPADB_BIN to the AOSP-built mtpadb executable}"
: "${MTPADB_DEVICE_SERIAL:?set MTPADB_DEVICE_SERIAL to one paired wireless ADB serial}"
ADB_BIN="${ADB_BIN:-adb}"

"$ADB_BIN" devices -l
state=$("$ADB_BIN" get-state -s "$MTPADB_DEVICE_SERIAL")
if [[ "$state" != "device" ]]; then
    echo "Selected serial is not an online ADB device: $MTPADB_DEVICE_SERIAL" >&2
    exit 1
fi

ping_result=$("$MTPADB_BIN" rpc --serial "$MTPADB_DEVICE_SERIAL" ping)
if [[ "$ping_result" != "PONG" ]]; then
    echo "MTPADB ping returned an unexpected response" >&2
    exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
payload="$tmpdir/payload.bin"
response="$tmpdir/response.bin"
dd if=/dev/zero of="$payload" bs=1048576 count=1 status=none
printf '\377\000\101\377' | dd of="$payload" bs=1 seek=0 conv=notrunc status=none
"$MTPADB_BIN" rpc --serial "$MTPADB_DEVICE_SERIAL" echo "$payload" >"$response"
cmp "$payload" "$response"
echo "MTPADB host ping and 1 MiB binary echo passed for $MTPADB_DEVICE_SERIAL"
