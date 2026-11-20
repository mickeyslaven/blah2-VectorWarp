#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/vectorwarp-artifact-commit.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
# Isolate provenance tests from the developer's intentionally dirty checkout.
fixture="$temporary/source"
git init -q "$fixture"
printf 'fixture\n' >"$fixture/source.txt"
git -C "$fixture" add source.txt
git -C "$fixture" -c user.name=Fixture -c user.email=fixture@example.invalid commit -qm fixture
commit=$(git -C "$fixture" rev-parse HEAD)
printf 'source_commit=%s\nsource_clean=true\n' "$commit" >"$temporary/.vectorwarp-build"
[[ $(bash "$source_dir/script/artifact-source-commit.sh" "$temporary" "$fixture") == "$commit" ]]
printf 'source_commit=%040d\nsource_clean=true\n' 0 >"$temporary/.vectorwarp-build"
if bash "$source_dir/script/artifact-source-commit.sh" "$temporary" "$fixture" 2>"$temporary/error"; then
  printf 'mismatched artifact source commit was accepted\n' >&2
  exit 1
fi
grep -q 'artifact source commit does not match packaging source checkout' "$temporary/error"
for state in false missing; do
  printf 'source_commit=%s\n' "$commit" >"$temporary/.vectorwarp-build"
  if [[ $state != missing ]]; then printf 'source_clean=%s\n' "$state" >>"$temporary/.vectorwarp-build"; fi
  if bash "$source_dir/script/artifact-source-commit.sh" "$temporary" "$fixture" 2>"$temporary/error"; then
    printf 'unverified artifact source cleanliness was accepted\n' >&2
    exit 1
  fi
  grep -q 'not built from a clean source checkout' "$temporary/error"
done
printf 'source_commit=%s\nsource_clean=true\n' "$commit" >"$temporary/.vectorwarp-build"
for change in tracked untracked; do
  if [[ $change == tracked ]]; then
    printf 'edited\n' >"$fixture/source.txt"
  else
    printf 'fixture\n' >"$fixture/source.txt"
    printf 'new source\n' >"$fixture/new.cpp"
  fi
  if bash "$source_dir/script/artifact-source-commit.sh" "$temporary" "$fixture" 2>"$temporary/error"; then
    printf 'dirty packaging source was accepted\n' >&2
    exit 1
  fi
  grep -q 'packaging source checkout is dirty' "$temporary/error"
done
printf 'artifact source commit binding passed\n'
