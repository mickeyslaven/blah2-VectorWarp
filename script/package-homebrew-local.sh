#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
out=${1:-"$root/build/homebrew-local"}
install_tap=${2:-}
[[ -z $install_tap || $install_tap == --install-tap ]] || { echo "usage: $0 [output-dir] [--install-tap]" >&2; exit 64; }
revision=${VECTORWARP_HOMEBREW_REVISION:-1}
[[ $revision =~ ^[1-9][0-9]*$ ]] || { echo 'VECTORWARP_HOMEBREW_REVISION must be a positive integer.' >&2; exit 64; }
mkdir -p "$out/source"
out=$(CDPATH= cd -P -- "$out" && pwd)
archive="$out/vectorwarp-source.tar.gz"
# A development source snapshot must include uncommitted port files but never
# ignored dependency trees, build products, or editor state. Git's tracked +
# non-ignored inventory is an explicit source allowlist.
git -C "$root" ls-files --cached --others --exclude-standard >"$out/source-files.txt"
tar -czf "$archive" -C "$root" -T "$out/source-files.txt"
sha=$(shasum -a 256 "$archive" | awk '{print $1}')
url="file://$archive"
for formula in vectorwarp vectorwarp-heimdall; do
  sed -e "s|@SOURCE_URL@|$url|" -e "s|@SOURCE_SHA256@|$sha|" \
    -e "s|@FORMULA_REVISION@|$revision|" "$root/Formula/$formula.rb.in" >"$out/$formula.rb"
done
if [[ $install_tap == --install-tap ]]; then
  command -v brew >/dev/null || { echo 'Homebrew is required to install the local tap.' >&2; exit 1; }
  tap="$(brew --repository)/Library/Taps/vectorwarp/homebrew-local"
  mkdir -p "$tap/Formula"
  [[ -d $tap/.git ]] || git -C "$tap" init -q
  install -m 0644 "$out/vectorwarp.rb" "$tap/Formula/vectorwarp.rb"
  install -m 0644 "$out/vectorwarp-heimdall.rb" "$tap/Formula/vectorwarp-heimdall.rb"
  printf 'Installed temporary local tap (no commit): vectorwarp/local\n'
fi
printf 'Local formula: %s\nInstall: brew install --build-from-source vectorwarp/local/vectorwarp\n' "$out/vectorwarp.rb"
