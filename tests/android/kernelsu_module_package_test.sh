#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'kernelsu_module_package_test.sh: %s\n' "$*" >&2
    exit 1
}

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
package_script="$repo_root/android/kernelsu/package-module.sh"

if ! "$package_script" --help >/dev/null 2>&1; then
    fail 'package help did not exit successfully'
fi
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

product_out="$tmp/product"
mkdir -p "$product_out/system/bin"
for binary in mtpadbd mtprpcd mtpadbctl; do
    printf 'fixture binary: %s\n' "$binary" >"$product_out/system/bin/$binary"
    chmod 0755 "$product_out/system/bin/$binary"
done

archive="$tmp/mtpadb-kernelsu.zip"
"$package_script" --product-out "$product_out" --output "$archive"
[[ -f "$archive" ]] || fail 'package archive was not created'

mapfile -t members < <(unzip -Z1 "$archive")
required_members=(
    module.prop
    customize.sh
    service.sh
    uninstall.sh
    initrc/mtpadb.rc
    bin/mtpadbd
    bin/mtprpcd
    bin/mtpadbctl
)
for required in "${required_members[@]}"; do
    found=false
    for member in "${members[@]}"; do
        if [[ "$member" == "$required" ]]; then
            found=true
            break
        fi
    done
    [[ "$found" == true ]] || fail "archive is missing $required"
done

for executable in bin/mtpadbd bin/mtprpcd bin/mtpadbctl service.sh uninstall.sh; do
    listing=$(zipinfo -l "$archive" "$executable")
    [[ "$listing" == *"-rwx"* ]] || fail "$executable is not executable in the archive"
done

rm -- "$product_out/system/bin/mtpadbctl"
missing_archive="$tmp/missing-binary.zip"
if "$package_script" --product-out "$product_out" --output "$missing_archive"; then
    fail 'packaging unexpectedly accepted a missing Android binary'
fi
[[ ! -e "$missing_archive" ]] || fail 'failed packaging left a partial archive'

printf 'KernelSU module package tests passed.\n'
