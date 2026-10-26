#!/usr/bin/env bash
set -euo pipefail

die() { printf 'repair-macos-homebrew-python-links: %s\n' "$*" >&2; exit 1; }

[[ $(uname -s) == Darwin && $(uname -m) == x86_64 ]] ||
  die 'this recovery is restricted to Intel macOS runners'
command -v brew >/dev/null 2>&1 || die 'Homebrew is unavailable'
brew list --versions python@3.12 >/dev/null 2>&1 || die 'python@3.12 is not installed'

# GitHub's Intel image can retain python.org framework links after Homebrew has
# poured python@3.12. Remove only that exact conflict set; refuse regular files
# and links owned by anything else.
for name in 2to3-3.12 idle3.12 pip3.12 pydoc3.12 python3.12 python3.12-config; do
  link="/usr/local/bin/$name"
  expected="/Library/Frameworks/Python.framework/Versions/3.12/bin/$name"
  if [[ -L $link ]]; then
    [[ $(readlink "$link") == "$expected" ]] || die "refusing to replace unexpected link: $link"
    unlink "$link"
  elif [[ -e $link ]]; then
    die "refusing to replace non-symlink: $link"
  fi
done

brew link python@3.12
