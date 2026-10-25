#!/usr/bin/env bash
# Resolve the newest published stable VectorWarp package older than the
# candidate. The GitHub release manifest is authoritative; filenames are not
# guessed from runner state.
set -euo pipefail

die() { printf 'download-previous-package: %s\n' "$*" >&2; exit 1; }
repo=mickeyslaven/blah2-VectorWarp
distro= arch= output= candidate_tag=
while (($#)); do
  case "$1" in
    --repo|--distro|--arch|--output|--candidate-tag)
      (($# >= 2)) || die "missing value for $1"
      case "$1" in
        --repo) repo=$2;; --distro) distro=$2;; --arch) arch=$2;;
        --output) output=$2;; --candidate-tag) candidate_tag=$2;;
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

case "$distro" in
  ubuntu22.04) family=ubuntu; version=22.04; format=deb ;;
  ubuntu24.04) family=ubuntu; version=24.04; format=deb ;;
  ubuntu26.04) family=ubuntu; version=26.04; format=deb ;;
  debian12) family=debian; version=12; format=deb ;;
  debian13) family=debian; version=13; format=deb ;;
  fedora44) family=fedora; version=44; format=rpm ;;
  *) die "unsupported distro: $distro" ;;
esac
filename=$(jq -er --arg format "$format" --arg family "$family" --arg version "$version" --arg arch "$arch" '
  [.packages[] | select(.format == $format and .distro == $family and
    .distro_version == $version and .arch == $arch)] |
  if length == 1 then .[0].filename else error("package matrix match was not unique") end' "$manifest") ||
  die "$tag has no unique $distro/$arch package"
asset_url=$(jq -er --arg filename "$filename" '.assets[] | select(.name == $filename) | .url' <<<"$release_json") ||
  die "$tag does not contain $filename"
gh api -H 'Accept: application/octet-stream' "$asset_url" >"$output/$filename"
printf 'tag=%s\npackage=%s\n' "$tag" "$output/$filename"
