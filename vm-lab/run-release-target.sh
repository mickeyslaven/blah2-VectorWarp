#!/usr/bin/env bash
# Controller-side launcher. CLEAN_DEST and UPGRADE_DEST must name two distinct
# disposable guests (user@host); this avoids destructive snapshot/reset logic.
set -euo pipefail
die() { printf 'run-release-target: %s\n' "$*" >&2; exit 1; }
target=${1:-}; clean_dest=${2:-}; upgrade_dest=${3:-}; source_archive=${4:-}; candidate=${5:-}; previous=${6:-}
[[ -n $target && -n $clean_dest && -n $upgrade_dest && -f $source_archive && -f $candidate && -f $previous ]] ||
  die 'usage: run-release-target.sh TARGET CLEAN_DEST UPGRADE_DEST SOURCE_TAR CANDIDATE PREVIOUS'
[[ $clean_dest != "$upgrade_dest" ]] || die 'clean and upgrade guests must be distinct disposable instances'
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
row=$(awk -F '\t' -v target="$target" '$1 == target {print; exit}' "$script_dir/release-matrix.tsv")
[[ -n $row ]] || die "unknown target: $target"
IFS=$'\t' read -r _ host guest expected_id expected_version expected_arch package_target memory execution <<<"$row"
if [[ $host == strix && ${VECTORWARP_QWEN_UNLOADED:-0} != 1 ]]; then
  die 'Strix release tests must run through vm-lab/with-qwen-unloaded.sh'
fi
if [[ $host == m2 ]]; then
  [[ $memory == 2048 && $execution == lima-serial ]] || die 'M2 guests must remain serial at 2 GiB'
  command -v limactl >/dev/null || die 'limactl is required on M2'
  actual_memory=$(limactl list --json | jq -er --arg name "$guest" 'select(.name == $name) | .memory' | head -1)
  [[ $actual_memory == 2147483648 ]] || die "$guest is not configured for exactly 2 GiB"
fi
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
install -m 0755 "$script_dir/guest-release-test.sh" "$work/guest-release-test.sh"
for mode in clean upgrade; do
  if [[ $mode == clean ]]; then dest=$clean_dest; else dest=$upgrade_dest; fi
  ssh "$dest" 'rm -rf /tmp/vectorwarp-release-input && mkdir -p /tmp/vectorwarp-release-input'
  scp "$work/guest-release-test.sh" "$source_archive" "$candidate" "$previous" "$dest:/tmp/vectorwarp-release-input/"
  source_name=${source_archive##*/}; candidate_name=${candidate##*/}; previous_name=${previous##*/}
  ssh "$dest" "cd /tmp/vectorwarp-release-input && tar -xf '$source_name' && source_dir=\$(find . -mindepth 1 -maxdepth 1 -type d -print -quit) && ./guest-release-test.sh --mode '$mode' --source \"\$source_dir\" --candidate '$candidate_name' --previous '$previous_name' --expected-id '$expected_id' --expected-version '$expected_version' --expected-arch '$expected_arch'"
done
