#!/usr/bin/env bash
# Prove APT can retain DragonOS FocalX's observed custom libuhd package while
# resolving a candidate. The fixture is metadata-only and never runs a fake
# library or replaces a real host package.
set -euo pipefail
candidate=${1:?candidate DEB required}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/pkg/DEBIAN"
cat >"$work/pkg/DEBIAN/control" <<'EOF'
Package: libuhd4.1.0
Version: 20230414-1
Architecture: amd64
Maintainer: root@dragon
Description: DragonOS custom UHD dependency-resolution fixture
EOF
dpkg-deb --build --root-owner-group "$work/pkg" "$work/libuhd4.1.0.deb" >/dev/null
dpkg --unpack "$work/libuhd4.1.0.deb" >/dev/null
DEBIAN_FRONTEND=noninteractive apt-get --simulate --no-install-recommends install "$candidate" >"$work/plan"
grep -Eq '^Inst vectorwarp ' "$work/plan"
! grep -Eq '^Inst (uhd-host|libuhd4\.1\.0) ' "$work/plan"
dpkg-query -W -f='${Version}\n' libuhd4.1.0 | grep -Fxq 20230414-1
printf 'DragonOS custom UHD remains selected; stock uhd-host/libuhd replacement is absent.\n'
