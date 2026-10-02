#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'package-module.sh: error: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf 'Usage: %s [--product-out DIR] [--output ZIP]\n' "$0"
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
product_out=${ANDROID_PRODUCT_OUT:-}
output=

while [[ $# -gt 0 ]]; do
    case "$1" in
        --product-out)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            product_out=$2
            shift 2
            ;;
        --output)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            output=$2
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

[[ -n "$product_out" ]] || fail 'set ANDROID_PRODUCT_OUT or pass --product-out'
[[ -d "$product_out/system/bin" ]] || fail "missing product binary directory: $product_out/system/bin"

for binary in mtpadbd mtprpcd mtpadbctl; do
    source_binary="$product_out/system/bin/$binary"
    [[ -f "$source_binary" && -x "$source_binary" ]] ||
        fail "missing executable AOSP output: $source_binary"
done

command -v zip >/dev/null 2>&1 || fail 'zip is required'
command -v unzip >/dev/null 2>&1 || fail 'unzip is required'

if [[ -z "$output" ]]; then
    output="$product_out/mtpadb-kernelsu.zip"
fi
output_dir=$(dirname -- "$output")
mkdir -p -- "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd -P)
output="$output_dir/$(basename -- "$output")"
[[ ! -e "$output" && ! -L "$output" ]] ||
    fail "refusing to overwrite existing output: $output"

temp_root=$(mktemp -d "$output_dir/.mtpadb-module.XXXXXX")
trap 'rm -rf -- "$temp_root"' EXIT
stage="$temp_root/module"
mkdir -p "$stage/bin" "$stage/initrc"

install -m 0644 "$script_dir/module.prop" "$stage/module.prop"
install -m 0755 "$script_dir/customize.sh" "$stage/customize.sh"
install -m 0755 "$script_dir/service.sh" "$stage/service.sh"
install -m 0755 "$script_dir/uninstall.sh" "$stage/uninstall.sh"
install -m 0644 "$script_dir/initrc/mtpadb.rc" "$stage/initrc/mtpadb.rc"
for binary in mtpadbd mtprpcd mtpadbctl; do
    install -m 0755 "$product_out/system/bin/$binary" "$stage/bin/$binary"
done

archive="$temp_root/mtpadb-kernelsu.zip"
(
    cd -- "$stage"
    zip -q -X -r "$archive" module.prop customize.sh service.sh uninstall.sh initrc bin
)
unzip -tq "$archive" >/dev/null || fail 'generated archive failed integrity validation'
ln -- "$archive" "$output" || fail "unable to publish archive: $output"
printf 'Created %s\n' "$output"
