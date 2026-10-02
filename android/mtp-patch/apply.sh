#!/usr/bin/env bash
set -eo pipefail
export LC_ALL=C

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

declare -a touched_paths=()
for patch in "${patches[@]}"; do
    while IFS=$'\t' read -r _ _ path; do
        [[ -n "${path:-}" ]] && touched_paths+=("$path")
    done < <(git -C "$adb_repo" apply --numstat "$patch")
done

simulation_dir=$(mktemp -d)
temporary_index=
trap 'rm -rf -- "$simulation_dir"; [[ -z "$temporary_index" ]] || rm -f -- "$temporary_index"' EXIT
for path in "${touched_paths[@]}"; do
    mkdir -p "$simulation_dir/$(dirname -- "$path")"
    if [[ -e "$adb_repo/$path" || -L "$adb_repo/$path" ]]; then
        cp -a -- "$adb_repo/$path" "$simulation_dir/$path"
    fi
done

all_applied=true
for ((index = ${#patches[@]} - 1; index >= 0; index--)); do
    patch=${patches[index]}
    if ! (cd -- "$simulation_dir" && git apply --reverse --check "$patch") >/dev/null 2>&1; then
        all_applied=false
        break
    fi
    (cd -- "$simulation_dir" && git apply --reverse "$patch")
done

if [[ "$all_applied" == true ]]; then
    for patch in "${patches[@]}"; do
        printf 'already applied: %s\n' "$(basename -- "$patch")"
    done
    exit 0
fi

if ! git -C "$adb_repo" diff --quiet HEAD -- "${touched_paths[@]}"; then
    fail 'patch series is partially applied or touched ADB files have local changes; no files changed'
fi
for path in "${touched_paths[@]}"; do
    if [[ -e "$adb_repo/$path" ]] &&
       ! git -C "$adb_repo" ls-files --error-unmatch -- "$path" >/dev/null 2>&1; then
        fail "untracked ADB file conflicts with patch series: $path"
    fi
done

temporary_index=$(mktemp)
GIT_INDEX_FILE="$temporary_index" git -C "$adb_repo" read-tree HEAD
for patch in "${patches[@]}"; do
    if ! GIT_INDEX_FILE="$temporary_index" git -C "$adb_repo" apply --cached "$patch"; then
        fail "patch series does not apply sequentially; no files changed: $(basename -- "$patch")"
    fi
done

for patch in "${patches[@]}"; do
    git -C "$adb_repo" apply "$patch"
    printf 'applied: %s\n' "$(basename -- "$patch")"
done
