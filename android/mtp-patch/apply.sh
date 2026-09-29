#!/usr/bin/env bash
set -eo pipefail

fail() {
    printf 'apply.sh: error: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf 'Usage: %s {android-11|modular-adb}\n' "$0" >&2
    exit 2
}

[[ $# -eq 1 ]] || usage
[[ -n "${ANDROID_BUILD_TOP:-}" ]] || fail 'ANDROID_BUILD_TOP must name the AOSP checkout root'
[[ -d "$ANDROID_BUILD_TOP" ]] || fail "ANDROID_BUILD_TOP is not a directory: $ANDROID_BUILD_TOP"

build_top=$(cd -- "$ANDROID_BUILD_TOP" && pwd -P)
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
project_root=$(cd -- "$script_dir/../.." && pwd -P)

case "$1" in
    android-11)
        adb_repo="$build_top/system/core"
        patch_dir="$project_root/android/mtp-patch/android-11"
        expected_revision=348efca472d810d3152568913da41a081893a4e3
        required_sources=(adb/Android.bp adb/daemon/main.cpp)
        ;;
    modular-adb)
        adb_repo="$build_top/packages/modules/adb"
        patch_dir="$project_root/android/mtp-patch/modular-adb"
        expected_revision=1cf2f017d312f73b3dc53bda85ef2610e35a80e9
        required_sources=(Android.bp daemon/main.cpp)
        ;;
    *)
        usage
        ;;
esac

[[ -d "$adb_repo" ]] || fail "expected ADB source checkout is missing: $adb_repo"
for source in "${required_sources[@]}"; do
    [[ -f "$adb_repo/$source" ]] || fail "expected ADB source file is missing: $adb_repo/$source"
done

expected_root=$(cd -- "$adb_repo" && pwd -P)
if ! actual_root=$(git -C "$adb_repo" rev-parse --show-toplevel 2>/dev/null); then
    fail "ADB source path is not a Git checkout: $adb_repo"
fi
actual_root=$(cd -- "$actual_root" && pwd -P)
[[ "$actual_root" == "$expected_root" ]] || fail "unexpected Git root for ADB source: $actual_root (expected $expected_root)"

if ! actual_revision=$(git -C "$adb_repo" rev-parse --verify HEAD 2>/dev/null); then
    fail "cannot resolve HEAD for ADB source checkout: $adb_repo"
fi
[[ "$actual_revision" == "$expected_revision" ]] || fail "wrong ADB revision in $adb_repo: expected $expected_revision, got $actual_revision"

[[ -d "$patch_dir" ]] || fail "patch directory is missing: $patch_dir"
shopt -s nullglob
patches=("$patch_dir"/*.patch)
[[ ${#patches[@]} -gt 0 ]] || fail "no patch files found in $patch_dir"

declare -a pending=()
declare -a applied=()
declare -a invalid=()
for patch in "${patches[@]}"; do
    if git -C "$adb_repo" apply --check "$patch" >/dev/null 2>&1; then
        pending+=("$patch")
    elif git -C "$adb_repo" apply --reverse --check "$patch" >/dev/null 2>&1; then
        applied+=("$patch")
    else
        invalid+=("$patch")
    fi
done

if [[ ${#invalid[@]} -gt 0 ]]; then
    for patch in "${invalid[@]}"; do
        printf 'apply.sh: patch does not cleanly apply or reverse: %s\n' "$(basename -- "$patch")" >&2
    done
    fail 'no files changed; resolve the patch/source mismatch before retrying'
fi

if [[ ${#pending[@]} -gt 0 && ${#applied[@]} -gt 0 ]]; then
    fail 'patch series is only partially applied; no files changed, restore the pinned source and apply the complete series'
fi

if [[ ${#applied[@]} -eq ${#patches[@]} ]]; then
    for patch in "${patches[@]}"; do
        printf 'already applied: %s\n' "$(basename -- "$patch")"
    done
    exit 0
fi

for patch in "${patches[@]}"; do
    git -C "$adb_repo" apply "$patch"
    printf 'applied: %s\n' "$(basename -- "$patch")"
done
