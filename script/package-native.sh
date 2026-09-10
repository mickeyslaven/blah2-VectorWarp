#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
ARTIFACT="$SOURCE_DIR/build/native/artifact"
NODE_RUNTIME=
OUTPUT_DIR="$SOURCE_DIR/dist"
FORMAT=
DISTRO=
VERSION=
PACKAGE_RELEASE=1
DRY_RUN=false
PREFLIGHT_ONLY=false

usage() {
  cat <<'EOF'
Usage: script/package-native.sh [options]

Build a distro-native VectorWarp release package from a native artifact.

  --format deb|rpm        Package format
  --distro NAME           ubuntu22.04, ubuntu24.04, ubuntu26.04, debian13 or fedora44
  --version X.Y.Z         Stable release version (without v)
  --artifact PATH         Artifact made by build-native.sh
  --node-runtime PATH     Extracted official Node.js 24.21.0 Linux archive
  --output-dir PATH       Destination for package and manifest (default: dist)
  --preflight             Validate all inputs and tools, then stop
  --dry-run               Print the staging plan without writing a package
  -h, --help              Show this help

This script does not install dependencies, fetch files, modify the host, sign a
package, or start services. Run it inside the exact distribution named by
--distro. Release CI signs packages and repository metadata separately.
EOF
}

die() { printf 'package-native: %s\n' "$*" >&2; exit 1; }
say() { printf 'package-native: %s\n' "$*"; }
quote_command() { printf ' %q' "$@"; printf '\n'; }
run() { if $DRY_RUN; then printf '+'; quote_command "$@"; else "$@"; fi; }
need_command() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1"; }

while (($#)); do
  case "$1" in
    --format) (($# >= 2)) || die '--format needs a value'; FORMAT=$2; shift 2 ;;
    --distro) (($# >= 2)) || die '--distro needs a value'; DISTRO=$2; shift 2 ;;
    --version) (($# >= 2)) || die '--version needs a value'; VERSION=$2; shift 2 ;;
    --artifact) (($# >= 2)) || die '--artifact needs a value'; ARTIFACT=$2; shift 2 ;;
    --node-runtime) (($# >= 2)) || die '--node-runtime needs a value'; NODE_RUNTIME=$2; shift 2 ;;
    --output-dir) (($# >= 2)) || die '--output-dir needs a value'; OUTPUT_DIR=$2; shift 2 ;;
    --preflight) PREFLIGHT_ONLY=true; shift ;;
    --dry-run) DRY_RUN=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ $FORMAT == deb || $FORMAT == rpm ]] || die '--format must be deb or rpm'
[[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die '--version must be a stable X.Y.Z version'
PACKAGE_VERSION=$VERSION
[[ -n $NODE_RUNTIME ]] || die '--node-runtime is required'
ARTIFACT=$(realpath -m "$ARTIFACT")
NODE_RUNTIME=$(realpath -m "$NODE_RUNTIME")
OUTPUT_DIR=$(realpath -m "$OUTPUT_DIR")
[[ $OUTPUT_DIR != / && $OUTPUT_DIR != "$SOURCE_DIR" ]] || die 'unsafe output directory'

case "$(uname -m)" in
  x86_64) DEB_ARCH=amd64; RPM_ARCH=x86_64; NODE_ARCH=x64 ;;
  aarch64|arm64) DEB_ARCH=arm64; RPM_ARCH=aarch64; NODE_ARCH=arm64 ;;
  *) die "unsupported build architecture: $(uname -m)" ;;
esac

[[ -r /etc/os-release ]] || die 'cannot identify the build distribution'
# This file contains simple distribution-owned assignments.
# shellcheck disable=SC1091
. /etc/os-release
VERSION=$PACKAGE_VERSION
case "$DISTRO" in
  ubuntu22.04)
    [[ $FORMAT == deb && ${ID:-} == ubuntu && ${VERSION_ID:-} == 22.04 ]] ||
      die 'ubuntu22.04 DEBs must be built on Ubuntu 22.04'
    DISTRO_NAME=ubuntu; DISTRO_VERSION=22.04; CODENAME=jammy; PACKAGE_ARCH=$DEB_ARCH ;;
  ubuntu24.04)
    [[ $FORMAT == deb && ${ID:-} == ubuntu && ${VERSION_ID:-} == 24.04 ]] ||
      die 'ubuntu24.04 DEBs must be built on Ubuntu 24.04'
    DISTRO_NAME=ubuntu; DISTRO_VERSION=24.04; CODENAME=noble; PACKAGE_ARCH=$DEB_ARCH ;;
  ubuntu26.04)
    [[ $FORMAT == deb && ${ID:-} == ubuntu && ${VERSION_ID:-} == 26.04 ]] ||
      die 'ubuntu26.04 DEBs must be built on Ubuntu 26.04'
    DISTRO_NAME=ubuntu; DISTRO_VERSION=26.04; CODENAME=resolute; PACKAGE_ARCH=$DEB_ARCH ;;
  debian13)
    [[ $FORMAT == deb && ${ID:-} == debian && ${VERSION_ID:-} == 13 && ${VERSION_CODENAME:-} == trixie ]] ||
      die 'debian13 DEBs must be built on Debian 13 Trixie'
    DISTRO_NAME=debian; DISTRO_VERSION=13; CODENAME=trixie; PACKAGE_ARCH=$DEB_ARCH ;;
  fedora44)
    [[ $FORMAT == rpm && ${ID:-} == fedora && ${VERSION_ID:-} == 44 ]] ||
      die 'fedora44 RPMs must be built in Fedora 44 userspace'
    DISTRO_NAME=fedora; DISTRO_VERSION=44; CODENAME=; PACKAGE_ARCH=$RPM_ARCH ;;
  *) die '--distro must be ubuntu22.04, ubuntu24.04, ubuntu26.04, debian13 or fedora44' ;;
esac

for command in file realpath sha256sum stat tar visudo; do need_command "$command"; done
for file in .vectorwarp-build bin/blah2 bin/blah2-gpu-worker bin/blah2-gpu-vulkan.so \
  api/server.js html/index.html config-examples/config-kraken.yml; do
  [[ -e $ARTIFACT/$file ]] || die "release artifact is incomplete: $file"
done
for notice in asio cpp-httplib rapidjson ryml VkFFT; do
  [[ -s $ARTIFACT/licenses/$notice.txt ]] || die "artifact lacks third-party notice: $notice"
done
[[ -x $ARTIFACT/bin/blah2 && -x $ARTIFACT/bin/blah2-gpu-worker ]] ||
  die 'release processor binaries are not executable'
backend=$(sed -n 's/^backend=//p' "$ARTIFACT/.vectorwarp-build")
gpu=$(sed -n 's/^gpu=//p' "$ARTIFACT/.vectorwarp-build")
build_os_id=$(sed -n 's/^build_os_id=//p' "$ARTIFACT/.vectorwarp-build")
build_os_version=$(sed -n 's/^build_os_version=//p' "$ARTIFACT/.vectorwarp-build")
build_arch=$(sed -n 's/^build_arch=//p' "$ARTIFACT/.vectorwarp-build")
[[ $backend == kraken ]] || die 'published packages require the Kraken live backend'
[[ $gpu == AUTO ]] || die 'published packages require the CPU plus Vulkan AUTO build'
[[ $build_os_id == "${ID:-}" && $build_os_version == "${VERSION_ID:-}" && $build_arch == "$(uname -m)" ]] ||
  die 'artifact was not built natively on this exact distribution and architecture'

processor_file=$(file -Lb "$ARTIFACT/bin/blah2")
case "$NODE_ARCH" in
  x64) [[ $processor_file == *x86-64* || $processor_file == *x86_64* ]] || die 'processor architecture mismatch' ;;
  arm64) [[ $processor_file == *aarch64* || $processor_file == *ARM\ aarch64* ]] || die 'processor architecture mismatch' ;;
esac

[[ -x $NODE_RUNTIME/bin/node && -f $NODE_RUNTIME/LICENSE ]] ||
  die 'Node runtime must contain executable bin/node and LICENSE'
node_version=$($NODE_RUNTIME/bin/node --version 2>/dev/null || true)
[[ $node_version == v24.21.0 ]] || die "Node runtime must be v24.21.0 (found ${node_version:-unusable})"
node_file=$(file -Lb "$NODE_RUNTIME/bin/node")
case "$NODE_ARCH" in
  x64) [[ $node_file == *x86-64* || $node_file == *x86_64* ]] || die 'Node runtime architecture mismatch' ;;
  arm64) [[ $node_file == *aarch64* || $node_file == *ARM\ aarch64* ]] || die 'Node runtime architecture mismatch' ;;
esac

if [[ $FORMAT == deb ]]; then
  for command in dpkg-deb dpkg-shlibdeps; do need_command "$command"; done
else
  need_command rpmbuild
  need_command rpm
fi

say "native target: $DISTRO ($PACKAGE_ARCH)"
say "artifact: $ARTIFACT"
say "private runtime: Node.js ${node_version#v} ($NODE_ARCH)"
say 'services will remain disabled and stopped'
if $PREFLIGHT_ONLY; then say 'preflight passed'; exit 0; fi
if $DRY_RUN; then
  say "would stage with install-native.sh and write package to $OUTPUT_DIR"
  exit 0
fi

run mkdir -p "$OUTPUT_DIR"
if [[ $FORMAT == deb ]]; then
  EXPECTED_ASSET="vectorwarp_${VERSION}-${PACKAGE_RELEASE}_${DISTRO}_${DEB_ARCH}.deb"
else
  EXPECTED_ASSET="vectorwarp-${VERSION}-${PACKAGE_RELEASE}.fc44.${RPM_ARCH}.rpm"
fi
for destination in "$OUTPUT_DIR/$EXPECTED_ASSET" "$OUTPUT_DIR/$EXPECTED_ASSET.manifest.json"; do
  [[ ! -e $destination && ! -L $destination ]] ||
    die "refusing to replace existing output: $destination"
done
WORK_DIR=$(mktemp -d "$OUTPUT_DIR/.vectorwarp-package.XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT
STAGE="$WORK_DIR/root"
run mkdir -p "$STAGE"
run "$SOURCE_DIR/script/install-native.sh" --artifact "$ARTIFACT" --destdir "$STAGE"

NODE_TARGET="$STAGE/opt/vectorwarp/runtime/node"
run install -d -m 0755 "$NODE_TARGET/bin"
run install -m 0755 "$NODE_RUNTIME/bin/node" "$NODE_TARGET/bin/node"
run install -m 0644 "$NODE_RUNTIME/LICENSE" "$NODE_TARGET/LICENSE"

# The source/native installer intentionally uses the administrator's system
# Node. Binary release packages instead use the verified private runtime.
sed -i 's|ExecStart=/usr/bin/node |ExecStart=/opt/vectorwarp/runtime/node/bin/node |' \
  "$STAGE/usr/lib/systemd/system/vectorwarp-api.service"
sed -i 's|^/usr/bin/node /opt/vectorwarp/|/opt/vectorwarp/runtime/node/bin/node /opt/vectorwarp/|' \
  "$STAGE/opt/vectorwarp/libexec/vectorwarp-restart"
grep -q '^ExecStart=/opt/vectorwarp/runtime/node/bin/node ' \
  "$STAGE/usr/lib/systemd/system/vectorwarp-api.service" || die 'could not bind API unit to private Node'
grep -q '^/opt/vectorwarp/runtime/node/bin/node ' \
  "$STAGE/opt/vectorwarp/libexec/vectorwarp-restart" || die 'could not bind restart helper to private Node'
visudo -cf "$STAGE/etc/sudoers.d/vectorwarp" >/dev/null

# The rendered integration files above replace the templates/helpers inside the
# release payload. npm command shims and executable CLI scripts are not used by
# the API; leaving them executable makes RPM infer a false /usr/bin/node
# dependency even though the verified private runtime is authoritative.
run rm -rf "$STAGE/opt/vectorwarp/current/systemd" \
  "$STAGE/opt/vectorwarp/current/libexec" \
  "$STAGE/opt/vectorwarp/current/api/node_modules/.bin"
run find "$STAGE/opt/vectorwarp/current/api/node_modules" -type f -exec chmod a-x '{}' +
run chmod 0644 "$STAGE/opt/vectorwarp/libexec/vectorwarp-wait-api.js"

printf 'package=vectorwarp\nversion=%s\nrelease=%s\ndistro=%s\narchitecture=%s\nnode=%s\n' \
  "$VERSION" "$PACKAGE_RELEASE" "$DISTRO" "$PACKAGE_ARCH" "${node_version#v}" \
  >"$STAGE/opt/vectorwarp/PACKAGE-METADATA"

if [[ $FORMAT == deb ]]; then
  CONTROL="$STAGE/DEBIAN"
  install -d -m 0755 "$CONTROL" "$WORK_DIR/debian"
  # dpkg-shlibdeps requires package metadata in its working directory and
  # derives the exact ABI package names of this distribution from the ELF set.
  printf 'Source: vectorwarp\nSection: hamradio\nPriority: optional\nMaintainer: Mickey Slaven <mickeyslaven@gmail.com>\nStandards-Version: 4.6.2\n\nPackage: vectorwarp\nArchitecture: %s\nDescription: VectorWarp\n' \
    "$DEB_ARCH" >"$WORK_DIR/debian/control"
  binaries=(
    "$STAGE/opt/vectorwarp/current/bin/blah2"
    "$STAGE/opt/vectorwarp/current/bin/blah2-gpu-worker"
    "$STAGE/opt/vectorwarp/current/bin/blah2-gpu-vulkan.so"
    "$STAGE/opt/vectorwarp/runtime/node/bin/node"
  )
  shlib_args=()
  for binary in "${binaries[@]}"; do shlib_args+=("-e$binary"); done
  shlibs_output=$(cd "$WORK_DIR" && dpkg-shlibdeps -O "${shlib_args[@]}")
  shlibs=${shlibs_output#shlibs:Depends=}
  [[ -n $shlibs && $shlibs != "$shlibs_output" ]] || die 'could not derive Debian runtime dependencies'
  installed_size=$(du -sk "$STAGE" | awk '{print $1}')
  printf 'Package: vectorwarp\nVersion: %s-%s\nArchitecture: %s\nMaintainer: Mickey Slaven <mickeyslaven@gmail.com>\nInstalled-Size: %s\nDepends: %s, systemd, sudo\nSection: hamradio\nPriority: optional\nHomepage: https://github.com/mickeyslaven/blah2-VectorWarp\nDescription: Native passive-radar processor and web interface\n VectorWarp supports live Kraken/Heimdall input and replay of recordings from\n all four receiver formats. Radar processing is never started by installation.\n' \
    "$VERSION" "$PACKAGE_RELEASE" "$DEB_ARCH" "$installed_size" "$shlibs" >"$CONTROL/control"
  printf '/etc/vectorwarp/config.yml\n/etc/sudoers.d/vectorwarp\n' >"$CONTROL/conffiles"
  install -m 0755 "$SOURCE_DIR/packaging/deb/postinst" "$CONTROL/postinst"
  install -m 0755 "$SOURCE_DIR/packaging/deb/prerm" "$CONTROL/prerm"
  install -m 0755 "$SOURCE_DIR/packaging/deb/postrm" "$CONTROL/postrm"
  ASSET="vectorwarp_${VERSION}-${PACKAGE_RELEASE}_${DISTRO}_${DEB_ARCH}.deb"
  dpkg-deb --build --root-owner-group "$STAGE" "$WORK_DIR/$ASSET"
  deb_identity="$(dpkg-deb -f "$WORK_DIR/$ASSET" Package) $(dpkg-deb -f "$WORK_DIR/$ASSET" Version) $(dpkg-deb -f "$WORK_DIR/$ASSET" Architecture)"
  [[ $deb_identity == "vectorwarp $VERSION-$PACKAGE_RELEASE $DEB_ARCH" ]] ||
    die "Debian package identity mismatch: $deb_identity"
  MANIFEST_RELEASE=$PACKAGE_RELEASE
else
  TOPDIR="$WORK_DIR/rpmbuild"
  install -d "$TOPDIR/BUILD" "$TOPDIR/BUILDROOT" "$TOPDIR/RPMS" "$TOPDIR/SOURCES" "$TOPDIR/SPECS" "$TOPDIR/SRPMS"
  tar -C "$STAGE" -cf "$TOPDIR/SOURCES/vectorwarp-root.tar" .
  RPM_RELEASE="${PACKAGE_RELEASE}.fc44"
  sed -e "s|@VERSION@|$VERSION|g" -e "s|@RPM_RELEASE@|$RPM_RELEASE|g" \
    "$SOURCE_DIR/packaging/rpm/vectorwarp.spec.in" >"$TOPDIR/SPECS/vectorwarp.spec"
  rpmbuild --define "_topdir $TOPDIR" --define "_arch $RPM_ARCH" -bb "$TOPDIR/SPECS/vectorwarp.spec"
  rpm_file=$(find "$TOPDIR/RPMS" -type f -name 'vectorwarp-*.rpm' ! -name '*debuginfo*' -print -quit)
  [[ -n $rpm_file ]] || die 'rpmbuild did not produce a package'
  ASSET="vectorwarp-${VERSION}-${RPM_RELEASE}.${RPM_ARCH}.rpm"
  mv "$rpm_file" "$WORK_DIR/$ASSET"
  rpm_identity=$(rpm -qp --qf '%{NAME} %{VERSION} %{RELEASE} %{ARCH}' "$WORK_DIR/$ASSET")
  [[ $rpm_identity == "vectorwarp $VERSION $RPM_RELEASE $RPM_ARCH" ]] ||
    die "RPM package identity mismatch: $rpm_identity"
  if rpm -qp --requires "$WORK_DIR/$ASSET" | grep -Fxq /usr/bin/node; then
    die 'RPM incorrectly depends on system Node instead of its private runtime'
  fi
  MANIFEST_RELEASE=$RPM_RELEASE
fi

sha256=$(sha256sum "$WORK_DIR/$ASSET" | awk '{print $1}')
size=$(stat -c %s "$WORK_DIR/$ASSET")
manifest="$WORK_DIR/$ASSET.manifest.json"
{
  printf '{\n'
  printf '  "name": "vectorwarp",\n'
  printf '  "version": "%s",\n' "$VERSION"
  printf '  "release": "%s",\n' "$MANIFEST_RELEASE"
  printf '  "format": "%s",\n' "$FORMAT"
  printf '  "distro": "%s",\n' "$DISTRO_NAME"
  printf '  "distro_version": "%s",\n' "$DISTRO_VERSION"
  if [[ -n $CODENAME ]]; then printf '  "codename": "%s",\n' "$CODENAME"; fi
  printf '  "arch": "%s",\n' "$PACKAGE_ARCH"
  printf '  "filename": "%s",\n' "$ASSET"
  printf '  "sha256": "%s",\n' "$sha256"
  printf '  "size": %s,\n' "$size"
  printf '  "backend": "kraken",\n'
  printf '  "gpu": "auto",\n'
  printf '  "node_version": "24.21.0"\n'
  printf '}\n'
} >"$manifest"

# WORK_DIR is deliberately created inside OUTPUT_DIR, so hard links provide an
# atomic no-clobber publish on the same filesystem. Roll back only the package
# link created by this invocation if publishing its sidecar loses a race.
ln "$WORK_DIR/$ASSET" "$OUTPUT_DIR/$ASSET" ||
  die "refusing to replace existing output: $OUTPUT_DIR/$ASSET"
if ! ln "$manifest" "$OUTPUT_DIR/$ASSET.manifest.json"; then
  rm -f -- "$OUTPUT_DIR/$ASSET"
  die "refusing to replace existing output: $OUTPUT_DIR/$ASSET.manifest.json"
fi
say "package ready: $OUTPUT_DIR/$ASSET"
say "manifest ready: $OUTPUT_DIR/$ASSET.manifest.json"
say 'package is unsigned; release CI must sign and re-hash it before publication'
