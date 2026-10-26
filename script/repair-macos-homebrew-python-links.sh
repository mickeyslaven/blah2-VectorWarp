#!/usr/bin/env bash
set -euo pipefail

die() { printf 'repair-macos-homebrew-python-links: %s\n' "$*" >&2; exit 1; }

normalize_link_target() {
  /usr/bin/python3 - "$1" "$2" <<'PY'
import os
import sys
print(os.path.normpath(os.path.join(os.path.dirname(sys.argv[1]), sys.argv[2])))
PY
}

validate_link_target_text() {
  local link=$1 actual=$2 expected=$3 normalized detail
  normalized=$(normalize_link_target "$link" "$actual") || return
  if [[ $normalized != "$expected" ]]; then
    printf -v detail 'actual=%q normalized=%q expected=%q' "$actual" "$normalized" "$expected"
    printf '%s\n' "$detail" >&2
    return 1
  fi
}

main() {
  local name link expected actual
  [[ $(uname -s) == Darwin && $(uname -m) == x86_64 ]] ||
    die 'this recovery is restricted to Intel macOS runners'
  command -v brew >/dev/null 2>&1 || die 'Homebrew is unavailable'

  # GitHub's Intel image can retain python.org framework links before Homebrew
  # pours python@3.12. Remove only that exact conflict set before installation;
  # refuse regular files and links owned by anything else. Waiting until `brew
  # install` returns is too late because it may complete the VectorWarp install
  # after reporting the Python link warning and alter the conflicting targets.
  for name in 2to3-3.12 idle3.12 pip3.12 pydoc3.12 python3.12 python3.12-config; do
    link="/usr/local/bin/$name"
    expected="/Library/Frameworks/Python.framework/Versions/3.12/bin/$name"
    if [[ -L $link ]]; then
      actual=$(readlink "$link")
      validate_link_target_text "$link" "$actual" "$expected" ||
        die "refusing to replace unexpected link: $link"
      unlink "$link"
    elif [[ -e $link ]]; then
      die "refusing to replace non-symlink: $link"
    fi
  done
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then main "$@"; fi
