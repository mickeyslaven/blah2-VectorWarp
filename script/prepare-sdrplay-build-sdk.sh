#!/usr/bin/env bash
# Build inputs only: never execute the vendor installer or install its service.
set -euo pipefail
SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
die() { printf 'prepare-sdrplay-build-sdk: %s\n' "$*" >&2; exit 1; }
[[ $# == 2 && $1 == --output-dir ]] || die 'usage: prepare-sdrplay-build-sdk.sh --output-dir PATH'
[[ ${VECTORWARP_SDRPLAY_BUILD_LICENSE_ACCEPTED:-} == true ]] ||
  die 'SDRplay SDK build use requires explicit acceptance of its vendor license'
[[ ! -L $2 ]] || die 'SDK output directory must not be a symlink'
output=$(realpath -m "$2")
[[ $output != / && $output != "$SOURCE_DIR" && ! -L $output ]] || die 'unsafe SDK output directory'
if [[ -e $output ]]; then
  [[ -d $output && -z $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]] ||
    die 'SDK output directory must be new or empty'
fi
case "$(uname -m)" in
  x86_64) sdk_arch=amd64 ;;
  aarch64|arm64) sdk_arch=arm64 ;;
  *) die 'the approved build SDK supports x86-64 and ARM64 targets' ;;
esac
archive="$SOURCE_DIR/lib/sdrplay-3.15.2/SDRplay_RSP_API-Linux-3.15.2.run"
printf '%s  %s\n' 3a97ca764263bbe76fb0f2220e6408942357e8864c19e1408a6d6987af382fe3 "$archive" |
  sha256sum --check --strict >/dev/null || die 'SDK archive checksum mismatch'
mkdir -p -m 0700 "$output"
# This exact pinned Makeself file has a gzip/tar payload after 523 text lines.
# Whitelist headers and this architecture's link library; no scripts are run.
tail -n +524 "$archive" | gzip -dc | tar --no-same-owner --no-same-permissions \
  -xf - -C "$output" ./inc "./$sdk_arch/libsdrplay_api.so.3.15"
[[ -f $output/inc/sdrplay_api.h && -f $output/$sdk_arch/libsdrplay_api.so.3.15 ]] ||
  die 'SDK extraction did not produce the expected build inputs'
printf 'SDRplay build-only headers: %s/inc\nSDRplay build-only library: %s/%s/libsdrplay_api.so.3.15\n' \
  "$output" "$output" "$sdk_arch"
