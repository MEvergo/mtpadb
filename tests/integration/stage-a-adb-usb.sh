#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
binary=${MTPADB_GADGETD:-$repo_root/build/linux/mtpadb-gadgetd}
config=${MTPADB_CONFIG:-$repo_root/linux/host.conf.example}
adb_bin=${ADB:-adb}
adb_port=${MTPADB_TEST_ADB_PORT:-5038}
adb_server_pid=
trace_log=
adb_home=
gadget_pid=

if [ "$(id -u)" -ne 0 ]; then
    echo "run this test as root" >&2
    exit 2
fi
if [ ! -x "$binary" ]; then
    echo "gadget daemon not found or not executable: $binary" >&2
    exit 2
fi
command -v "$adb_bin" >/dev/null 2>&1 || {
    echo "adb not found: $adb_bin" >&2
    exit 2
}
adb_path=$(command -v "$adb_bin")
command -v ss >/dev/null 2>&1 || {
    echo "ss is required to check the isolated ADB server port" >&2
    exit 2
}
command -v awk >/dev/null 2>&1 || {
    echo "awk is required to compare adb serials literally" >&2
    exit 2
}
case "$adb_port" in
    ''|*[!0-9]*)
        echo "invalid MTPADB_TEST_ADB_PORT: $adb_port" >&2
        exit 2
        ;;
esac
if [ "$adb_port" -lt 1 ] || [ "$adb_port" -gt 65535 ]; then
    echo "MTPADB_TEST_ADB_PORT must be between 1 and 65535" >&2
    exit 2
fi
listeners=$(ss -H -ltn "sport = :$adb_port")
if [ -n "$listeners" ]; then
    echo "ADB test port is already in use: $adb_port" >&2
    exit 2
fi

adb_home=$(mktemp -d /tmp/mtpadb-adb-home.XXXXXX)
run_adb() {
    env -i \
        HOME="$adb_home" ANDROID_USER_HOME="$adb_home/.android" \
        PATH=/usr/bin:/bin ADB_MDNS=0 ADB_MDNS_AUTO_CONNECT=none \
        ADB_LOCAL_TRANSPORT_MAX_PORT=0 "$adb_path" "$@"
}
has_serial() {
    printf '%s\n' "$1" | awk -v expected="$2" '$1 == expected { found = 1 } END { exit !found }'
}
cleanup() {
    trap - EXIT
    cleanup_status=0
    if [ -n "$gadget_pid" ]; then
        kill -TERM "$gadget_pid" 2>/dev/null || true
        wait "$gadget_pid" 2>/dev/null || true
        if ! "$binary" cleanup >/dev/null; then
            echo "mtpadb-gadgetd cleanup failed" >&2
            cleanup_status=1
        fi
    fi
    if [ -n "$adb_server_pid" ] && kill -0 "$adb_server_pid" 2>/dev/null; then
        kill -TERM "$adb_server_pid" 2>/dev/null || true
        wait "$adb_server_pid" 2>/dev/null || true
    fi
    if [ -n "$trace_log" ] && [ -f "$trace_log" ]; then
        printf '\nADB_TRACE=usb log:\n' >&2
        cat "$trace_log" >&2 || true
        rm -f "$trace_log" || cleanup_status=1
    fi
    if [ -n "$adb_home" ] && [ -d "$adb_home" ]; then
        rm -rf -- "$adb_home" || cleanup_status=1
    fi
    if [ "$cleanup_status" -ne 0 ]; then
        exit "$cleanup_status"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"$binary" cleanup
trace_log=$(mktemp /tmp/mtpadb-adb-trace.XXXXXX)
env -i \
    HOME="$adb_home" ANDROID_USER_HOME="$adb_home/.android" \
    PATH=/usr/bin:/bin ADB_TRACE=usb ADB_MDNS=0 \
    ADB_MDNS_AUTO_CONNECT=none ADB_LOCAL_TRANSPORT_MAX_PORT=0 \
    "$adb_path" -P "$adb_port" nodaemon server >"$trace_log" 2>&1 &
adb_server_pid=$!

attempt=0
adb_server_ready=no
while [ "$attempt" -lt 100 ]; do
    if ! kill -0 "$adb_server_pid" 2>/dev/null; then
        wait "$adb_server_pid" || true
        echo "stock adb trace server exited during startup" >&2
        exit 1
    fi
    listeners=$(ss -H -ltnp "sport = :$adb_port")
    if [ -n "$listeners" ]; then
        if printf '%s\n' "$listeners" | grep -F "pid=$adb_server_pid," >/dev/null; then
            adb_server_ready=yes
            break
        fi
        echo "another process claimed ADB test port $adb_port" >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.1
done
if [ "$adb_server_ready" != yes ]; then
    echo "stock adb trace server did not open port $adb_port" >&2
    exit 1
fi

baseline=$(run_adb -P "$adb_port" devices -l)
"$binary" run --config "$config" &
gadget_pid=$!

attempt=0
udc_attribute=/sys/kernel/config/usb_gadget/mtpadb/UDC
while [ "$attempt" -lt 100 ]; do
    if [ -r "$udc_attribute" ] && [ -n "$(cat "$udc_attribute")" ] &&
       [ -e /dev/ffs-mtpadb/ep1 ] && [ -e /dev/ffs-mtpadb/ep2 ]; then
        break
    fi
    if ! kill -0 "$gadget_pid" 2>/dev/null; then
        wait "$gadget_pid" || true
        echo "gadget daemon exited before USB enumeration" >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.1
done
if [ ! -r "$udc_attribute" ] || [ -z "$(cat "$udc_attribute")" ] ||
   [ ! -e /dev/ffs-mtpadb/ep1 ] || [ ! -e /dev/ffs-mtpadb/ep2 ]; then
    echo "dummy_hcd gadget did not become ready" >&2
    exit 1
fi

expected_serial=$(cat /sys/kernel/config/usb_gadget/mtpadb/strings/0x409/serialnumber)
if has_serial "$baseline" "$expected_serial"; then
    echo "USB serial was already present before gadget startup: $expected_serial" >&2
    exit 1
fi

attempt=0
output=
while [ "$attempt" -lt 100 ]; do
    output=$(run_adb -P "$adb_port" devices -l)
    if has_serial "$output" "$expected_serial"; then
        printf '%s\n' "$output"
        exit 0
    fi
    if ! kill -0 "$gadget_pid" 2>/dev/null; then
        wait "$gadget_pid" || true
        echo "gadget daemon exited before USB enumeration" >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.1
done
if [ -n "$output" ]; then
    printf '%s\n' "$output"
fi
echo "stock adb did not report the newly connected USB serial: $expected_serial" >&2
exit 1
