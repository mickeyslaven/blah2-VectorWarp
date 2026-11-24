#!/usr/bin/env bash
# Resolve the newest published stable VectorWarp package older than the
# candidate. The GitHub release manifest is authoritative; filenames are not
# guessed from runner state.
set -euo pipefail

die() { printf 'download-previous-package: %s\n' "$*" >&2; exit 1; }
repo=mickeyslaven/blah2-VectorWarp
distro= arch= output= candidate_tag= candidate_version=
while (($#)); do
  case "$1" in
    --repo|--distro|--arch|--output|--candidate-tag|--candidate-version)
      (($# >= 2)) || die "missing value for $1"
      case "$1" in
        --repo) repo=$2;; --distro) distro=$2;; --arch) arch=$2;;
        --output) output=$2;; --candidate-tag) candidate_tag=$2;;
        --candidate-version) candidate_version=$2;;
      esac
      shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[[ -n $distro && -n $arch && -n $output ]] || die 'distro, arch and output are required'
command -v gh >/dev/null || die 'gh is required'
command -v jq >/dev/null || die 'jq is required'
mkdir -p "$output"

release_json=$(gh api --paginate "repos/$repo/releases?per_page=100" |
  jq -sc --arg candidate "$candidate_tag" '
    add | map(select(.draft == false and .prerelease == false and
      (.tag_name | test("^v[0-9]+\\.[0-9]+\\.[0-9]+$")) and
      (.tag_name != $candidate))) | sort_by(.published_at) | reverse | first')
tag=$(jq -er '.tag_name' <<<"$release_json") || die 'no previous stable package release exists'
manifest_url=$(jq -er '.assets[] | select(.name == "package-manifest.json") | .url' <<<"$release_json") ||
  die "$tag has no package manifest"
manifest="$output/previous-package-manifest.json"
gh api -H 'Accept: application/octet-stream' "$manifest_url" >"$manifest"

artifact_plan=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/artifact-plan.py
distribution=$(python3 "$artifact_plan" distribution "$distro") || exit 1
read -r family version format <<<"$distribution"
filename=$(jq -r --arg format "$format" --arg family "$family" --arg version "$version" --arg arch "$arch" '
  [.packages[] | select(.format == $format and .distro == $family and
    .distro_version == $version and .arch == $arch)] |
  if length == 1 then .[0].filename elif length == 0 then "" else error("package matrix match was not unique") end' "$manifest") ||
  die "$tag has no unique $distro/$arch package"
if [[ -z $filename ]]; then
  python3 "$(dirname "$artifact_plan")/../test/packaging/new_target_upgrade.py" prepare \
    --distro "$distro" --arch "$arch" --candidate "$candidate_version" --tag "$tag" \
    --manifest "$manifest" --output "$output/bootstrap-request.json" ||
    die "$tag has no package or qualified introduction baseline for $distro/$arch"
  printf 'tag=%s\nbootstrap_request=%s\n' "$tag" "$output/bootstrap-request.json"
  exit 0
fi
asset_url=$(jq -er --arg filename "$filename" '.assets[] | select(.name == $filename) | .url' <<<"$release_json") ||
  die "$tag does not contain $filename"
gh api -H 'Accept: application/octet-stream' "$asset_url" >"$output/$filename"
printf 'tag=%s\npackage=%s\n' "$tag" "$output/$filename"
