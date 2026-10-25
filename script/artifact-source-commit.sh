#!/usr/bin/env bash
set -euo pipefail

artifact=${1:?artifact path is required}
source_dir=${2:?source directory is required}
manifest="$artifact/.vectorwarp-build"
[[ -f $manifest ]] || { printf 'artifact-source-commit: artifact manifest is missing\n' >&2; exit 1; }
count=$(grep -c '^source_commit=' "$manifest" || true)
[[ $count == 1 ]] || { printf 'artifact-source-commit: artifact manifest must contain exactly one source_commit\n' >&2; exit 1; }
commit=$(sed -n 's/^source_commit=//p' "$manifest")
[[ $commit =~ ^[0-9a-f]{40}$ ]] || { printf 'artifact-source-commit: artifact source_commit is invalid\n' >&2; exit 1; }
source_commit=$(git -C "$source_dir" rev-parse HEAD)
[[ $commit == "$source_commit" ]] || {
  printf 'artifact-source-commit: artifact source commit does not match packaging source checkout\n' >&2
  exit 1
}
printf '%s\n' "$commit"
