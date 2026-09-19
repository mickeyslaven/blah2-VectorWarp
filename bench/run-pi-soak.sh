#!/usr/bin/env bash
# Run inside a detached systemd unit; preserve evidence and never restart a failure.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 4 ]]; then
  echo "Usage: $0 BINARY NEW_OUTPUT_DIRECTORY [auto|cpu] [SECONDS]" >&2
  exit 2
fi
binary=$(realpath "$1")
output=$2
mode=${3:-auto}
seconds=${4:-2400}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
[[ $(id -u) == 0 ]] || { echo 'Run as root for the perf flight recorder' >&2; exit 2; }
[[ -x "$binary" && "$mode" =~ ^(auto|cpu)$ && "$seconds" =~ ^[0-9]+$ ]] || exit 2
(( seconds > 0 && seconds <= 3600 )) || exit 2
command -v perf >/dev/null
# Qualified Pi 4 capture/CPU settings; recorded in every run below. Calling the
# Python harness directly does not supply these environment settings.
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/opt/vectorwarp/runtime/lib:/usr/local/lib}
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
export VECTORWARP_FFTW_PLAN=measure VECTORWARP_CLUTTER_WORKERS=2
export VECTORWARP_RSPDUO_CPI_QUEUE=1 VECTORWARP_RSPDUO_USB_MODE=bulk
export VECTORWARP_RSPDUO_COUNTER_SCALE=3
mkdir -- "$output" # Deliberately refuse existing evidence paths.
output=$(realpath "$output")
date -u +%FT%TZ > "$output/started-utc.txt"
uname -a > "$output/os.txt"
perf --version > "$output/perf-version.txt"
sha256sum "$binary" "$script_dir/live-rspduo-check.py" \
  "$script_dir/pi_spike_diagnostics.py" "$script_dir/run-pi-soak.sh" > "$output/hashes.sha256"
for artifact in "$(dirname "$binary")/blah2-mixed-worker" \
                "$(dirname "$binary")/blah2-receiver-rspduo.so" \
                "$(dirname "$binary")/libblah2-capture-core.so.1"; do
  if [[ -f "$artifact" ]]; then sha256sum "$artifact" >> "$output/hashes.sha256"; fi
done
# Record only performance variables, never unrelated credentials in the environment.
python3 - "$output/environment.json" <<'PY'
import json, os, sys
keys = [key for key in os.environ if key.startswith(('VECTORWARP_', 'BLAH2_BENCH_'))]
keys += ['OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'LD_LIBRARY_PATH']
with open(sys.argv[1], 'x') as stream:
    json.dump({key: os.environ[key] for key in sorted(set(keys)) if key in os.environ}, stream, indent=2)
PY
set +e
python3 "$script_dir/live-rspduo-check.py" --binary "$binary" --seconds "$seconds" \
  --output "$output/live" --acceleration "$mode" --spike-diagnostics --spike-ms 750 \
  > "$output/harness.log" 2>&1
result=$?
set -e
echo "$result" > "$output/harness-exit-code.txt"
# journalctl on Debian Bookworm does not accept the ISO-8601 value written
# above for --since.  Its documented @SECONDS form avoids locale/time-zone
# parsing and preserves the exact UTC start boundary.
started_utc=$(<"$output/started-utc.txt")
started_epoch=$(date -u -d "$started_utc" +%s)
journal_failed=0
if ! journalctl -k --since "@$started_epoch" -n 400 --no-pager \
  > "$output/kernel.log" 2>&1; then
  echo 'kernel journal collection failed; soak evidence is incomplete' >&2
  journal_failed=1
fi
if ! journalctl -u sdrplay --since "@$started_epoch" -n 600 --no-pager \
  > "$output/sdrplay.log" 2>&1; then
  echo 'SDRplay journal collection failed; soak evidence is incomplete' >&2
  journal_failed=1
fi
if (( journal_failed )); then
  printf 'failed epoch=%s\n' "$started_epoch" > "$output/journal-status.txt"
  wrapper_result=1
else
  printf 'ok epoch=%s\n' "$started_epoch" > "$output/journal-status.txt"
  wrapper_result=$result
fi
date -u +%FT%TZ > "$output/finished-utc.txt"
echo "$wrapper_result" > "$output/exit-code.txt"
exit "$wrapper_result"
