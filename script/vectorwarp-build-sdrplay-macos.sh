#!/bin/sh
# Fixed no-argument local adapter builder. The SDK is always manual.
set -eu
[ "$#" -eq 0 ] || { echo 'This builder accepts no arguments.' >&2; exit 64; }
ROOT=${VECTORWARP_MACOS_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)}
NODE=${VECTORWARP_MACOS_NODE:-node}
case "$ROOT" in /*) ;; *) exit 64;; esac
exec "$NODE" "$ROOT/script/vectorwarp-build-sdrplay-macos.js"
