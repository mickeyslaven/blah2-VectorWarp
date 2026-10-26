#!/usr/bin/env bash
# Strix guard: restore Qwen and require qwen-window.sh's health check to pass
# even when the test command fails or the controlling SSH session is signaled.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
((EUID == 0)) || { echo 'run as root' >&2; exit 2; }
(($#)) || { echo 'usage: with-qwen-unloaded.sh COMMAND [ARG ...]' >&2; exit 2; }
[[ -x $script_dir/qwen-window.sh ]] || { echo 'qwen-window.sh must be deployed beside this guard' >&2; exit 2; }
restored=false
restore() {
  $restored && return 0
  "$script_dir/qwen-window.sh" reload
  restored=true
}
trap 'restore' EXIT
trap 'status=$?; restore || status=1; trap - EXIT; exit "$status"' INT TERM HUP
"$script_dir/qwen-window.sh" unload
status=0
VECTORWARP_QWEN_UNLOADED=1 "$@" || status=$?
restore || status=1
trap - EXIT
exit "$status"
