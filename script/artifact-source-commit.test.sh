#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/vectorwarp-artifact-commit.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
commit=$(git -C "$source_dir" rev-parse HEAD)
printf 'source_commit=%s\n' "$commit" >"$temporary/.vectorwarp-build"
[[ $(bash "$source_dir/script/artifact-source-commit.sh" "$temporary" "$source_dir") == "$commit" ]]
printf 'source_commit=%040d\n' 0 >"$temporary/.vectorwarp-build"
if bash "$source_dir/script/artifact-source-commit.sh" "$temporary" "$source_dir" 2>"$temporary/error"; then
  printf 'mismatched artifact source commit was accepted\n' >&2
  exit 1
fi
grep -q 'artifact source commit does not match packaging source checkout' "$temporary/error"
printf 'artifact source commit binding passed\n'
