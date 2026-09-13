#!/usr/bin/env bash
# User-supplied build inputs only. No download, license acceptance or service action.
set -euo pipefail
SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
die() { printf 'prepare-sdrplay-build-sdk: %s\n' "$*" >&2; exit 1; }
[[ $# == 2 && $1 == --output-dir ]] || die 'usage: prepare-sdrplay-build-sdk.sh --output-dir PATH'
[[ ${VECTORWARP_SDRPLAY_BUILD_LICENSE_ACCEPTED:-} == true ]] ||
  die 'SDRplay SDK build use requires explicit acceptance of its vendor license'
[[ ! -L $2 ]] || die 'SDK output directory must not be a symlink'
output=$(realpath -m "$2")
[[ $output != / && $output != "$SOURCE_DIR" && $output != "$SOURCE_DIR/"* && ! -L $output ]] ||
  die 'SDK output must be a private directory outside the source tree'
if [[ -e $output ]]; then
  [[ -d $output && -z $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]] ||
    die 'SDK output directory must be new or empty'
  [[ $(stat -c %a -- "$output") == 700 && $(stat -c %u -- "$output") == "$(id -u)" ]] ||
    die 'Existing SDK output directory must be owned by this user with mode 0700'
fi
case "$(uname -m)" in
  x86_64) sdk_arch=amd64 ;;
  aarch64|arm64) sdk_arch=arm64 ;;
  *) die 'the approved build SDK supports x86-64 and ARM64 targets' ;;
esac
archive=${VECTORWARP_SDRPLAY_SDK_ARCHIVE:-}
outside_source() {
  local resolved
  [[ -r $1 && -f $1 ]] || die 'The selected licensed SDK input is not a readable file'
  resolved=$(realpath -e -- "$1")
  [[ $resolved != "$SOURCE_DIR/"* && $resolved != "$output" && $resolved != "$output/"* ]] ||
    die 'Licensed SDK inputs must be outside the source tree and output directory'
}
if [[ -n $archive ]]; then
  [[ $archive == /* ]] || die 'VECTORWARP_SDRPLAY_SDK_ARCHIVE must be an absolute local path'
  outside_source "$archive"
  printf '%s  %s\n' 3a97ca764263bbe76fb0f2220e6408942357e8864c19e1408a6d6987af382fe3 "$archive" |
    sha256sum --check --strict >/dev/null || die 'SDK archive checksum mismatch'
  mkdir -p -m 0700 "$output"
  # Exact vendor 3.15.2 archive only; never run its embedded installer.
  tail -n +524 "$archive" | gzip -dc | tar --no-same-owner --no-same-permissions \
    -xf - -C "$output" ./inc "./$sdk_arch/libsdrplay_api.so.3.15"
else
  include=${BLAH2_SDRPLAY_INCLUDE_DIR:-/usr/local/include}
  library=${BLAH2_SDRPLAY_LIBRARY:-/usr/local/lib/libsdrplay_api.so.3.15}
  [[ $include == /* && $library == /* ]] || die 'SDK include/library paths must be absolute'
  [[ -r $include/sdrplay_api.h && -r $library ]] ||
    die 'Licensed SDRplay SDK not found. Install it from https://sdrplay.com/hardware-api/ or explicitly supply external SDK paths. No repository/history fallback is used.'
  outside_source "$include/sdrplay_api.h"
  outside_source "$library"
  shopt -s nullglob
  headers=("$include"/sdrplay_api*.h)
  ((${#headers[@]} >= 1 && ${#headers[@]} <= 64)) || die 'Unexpected SDRplay header inventory'
  for header in "${headers[@]}"; do
    [[ $(basename -- "$header") =~ ^sdrplay_api[A-Za-z0-9_]*\.h$ ]] || die 'Unexpected SDK header name'
    outside_source "$header"
  done
  mkdir -p -m 0700 "$output"
  mkdir -p -m 0700 "$output/inc" "$output/$sdk_arch"
  for header in "${headers[@]}"; do cp -L -- "$header" "$output/inc/"; done
  cp -L -- "$library" "$output/$sdk_arch/libsdrplay_api.so.3.15"
fi
[[ -f $output/inc/sdrplay_api.h && -f $output/$sdk_arch/libsdrplay_api.so.3.15 ]] ||
  die 'SDK extraction did not produce the expected build inputs'
printf 'Licensed SDRplay build inputs prepared; keep this directory out of public artifacts and caches.\n'
