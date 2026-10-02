#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'prepare-aosp.sh: error: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf 'Usage: %s {android-11|modular-adb}\n' "$0" >&2
    exit 2
}

[[ $# -eq 1 ]] || usage
case "$1" in
    android-11|modular-adb) ;;
    *) usage ;;
esac

[[ -n "${ANDROID_BUILD_TOP:-}" ]] || fail 'ANDROID_BUILD_TOP must name the AOSP checkout root'
[[ -d "$ANDROID_BUILD_TOP" ]] || fail "ANDROID_BUILD_TOP is not a directory: $ANDROID_BUILD_TOP"

build_top=$(cd -- "$ANDROID_BUILD_TOP" && pwd -P)
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
external_dir="$build_top/external"
project_link="$external_dir/mtpadb"

[[ -d "$external_dir" ]] || fail "AOSP external directory is missing: $external_dir"
if [[ -e "$project_link" || -L "$project_link" ]]; then
    if ! linked_root=$(cd -- "$project_link" 2>/dev/null && pwd -P); then
        fail "external/mtpadb exists but is not a usable directory: $project_link"
    fi
    [[ "$linked_root" == "$project_root" ]] ||
        fail "external/mtpadb points to another checkout: $linked_root"
else
    ln -s -- "$project_root" "$project_link"
fi

"$project_root/android/mtp-patch/apply.sh" "$1"
printf 'Project source available at %s\n' "$project_link"
printf 'Include external/mtpadb/android/deploy/mtpadb_product.mk from the target product makefile.\n'
