#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
BUILD_DIR="$SOURCE_DIR/build/native"
DEPS_DIR="$SOURCE_DIR/build/native-deps"
BACKEND=kraken
GPU=AUTO
JOBS=4
OFFLINE=false
DRY_RUN=false
PREFLIGHT_ONLY=false
SDRPLAY_INCLUDE_DIR=${BLAH2_SDRPLAY_INCLUDE_DIR:-/usr/local/include}
SDRPLAY_LIBRARY=${BLAH2_SDRPLAY_LIBRARY:-/usr/local/lib/libsdrplay_api.so.3.15}

VCPKG_COMMIT=c8696863d371ab7f46e213d8f5ca923c4aef2a00
VKFFT_COMMIT=066a17c17068c0f11c9298d848c2976c71fad1c1

usage() {
  cat <<'EOF'
Usage: script/build-native.sh [options]

Build a relocatable VectorWarp artifact without installing it.

  --backend NAME          kraken (default), rspduo, usrp, hackrf or all
  --gpu auto|on|off       Optional Vulkan worker selection (default: auto)
  --build-dir PATH        Build/output directory (default: build/native)
  --deps-dir PATH         Pinned source dependency cache
  --jobs N                Parallel build jobs (default: 4)
  --offline               Never fetch; require both pinned checkouts locally
  --preflight             Check tools and host SDKs, then stop
  --dry-run               Print planned commands without changing files
  -h, --help              Show this help

The completed artifact is BUILD_DIR/artifact. Installation is a separate step.
EOF
}

die() { printf 'build-native: %s\n' "$*" >&2; exit 1; }
say() { printf 'build-native: %s\n' "$*"; }
quote_command() { printf ' %q' "$@"; printf '\n'; }
run() {
  if $DRY_RUN; then printf '+'; quote_command "$@"; else "$@"; fi
}
need_command() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1"; }

while (($#)); do
  case "$1" in
    --backend) (($# >= 2)) || die '--backend needs a value'; BACKEND=$2; shift 2 ;;
    --gpu) (($# >= 2)) || die '--gpu needs a value'; GPU=${2^^}; shift 2 ;;
    --build-dir) (($# >= 2)) || die '--build-dir needs a value'; BUILD_DIR=$2; shift 2 ;;
    --deps-dir) (($# >= 2)) || die '--deps-dir needs a value'; DEPS_DIR=$2; shift 2 ;;
    --jobs) (($# >= 2)) || die '--jobs needs a value'; JOBS=$2; shift 2 ;;
    --offline) OFFLINE=true; shift ;;
    --preflight) PREFLIGHT_ONLY=true; shift ;;
    --dry-run) DRY_RUN=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

case "$BACKEND" in
  kraken)
    ENABLE_RSPDUO=OFF; ENABLE_USRP=OFF; ENABLE_HACKRF=OFF
    COMPILED_RECEIVERS=Kraken ;;
  rspduo)
    ENABLE_RSPDUO=ON; ENABLE_USRP=OFF; ENABLE_HACKRF=OFF
    COMPILED_RECEIVERS=RspDuo,Kraken ;;
  usrp)
    ENABLE_RSPDUO=OFF; ENABLE_USRP=ON; ENABLE_HACKRF=OFF
    COMPILED_RECEIVERS=Usrp,Kraken ;;
  hackrf)
    ENABLE_RSPDUO=OFF; ENABLE_USRP=OFF; ENABLE_HACKRF=ON
    COMPILED_RECEIVERS=HackRF,Kraken ;;
  all)
    ENABLE_RSPDUO=ON; ENABLE_USRP=ON; ENABLE_HACKRF=ON
    COMPILED_RECEIVERS=RspDuo,Usrp,HackRF,Kraken ;;
  *) die '--backend must be kraken, rspduo, usrp, hackrf or all' ;;
esac
[[ $GPU =~ ^(AUTO|ON|OFF)$ ]] || die '--gpu must be auto, on or off'
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || die '--jobs must be a positive integer'
BUILD_DIR=$(realpath -m "$BUILD_DIR")
DEPS_DIR=$(realpath -m "$DEPS_DIR")
BUILD_ARCH=$(uname -m)
[[ $BUILD_DIR != / && $DEPS_DIR != / ]] || die 'refusing to use / as a work directory'
[[ $BUILD_DIR != "$SOURCE_DIR" && $DEPS_DIR != "$SOURCE_DIR" ]] ||
  die 'build and dependency directories must not replace the source tree'

for command in cmake git node npm curl tar zip unzip pkg-config c++ ninja; do need_command "$command"; done
node_major=$(node -p 'Number(process.versions.node.split(".")[0])')
((node_major >= 22)) || die 'Node.js 22 or newer is required'
for file in CMakeLists.txt lib/vcpkg.json lib/vcpkg-kraken.json api/package.json html/index.html; do
  [[ -f "$SOURCE_DIR/$file" ]] || die "source file is missing: $file"
done
pkg-config --exists fftw3 || die 'FFTW3 development files are required'
pkg-config --exists armadillo || die 'Armadillo development files are required'

if [[ $ENABLE_RSPDUO == ON ]]; then
  [[ -r $SDRPLAY_INCLUDE_DIR/sdrplay_api.h ]] ||
    die "rspduo backend needs the SDRplay 3.15 header at $SDRPLAY_INCLUDE_DIR/sdrplay_api.h"
  [[ -r $SDRPLAY_LIBRARY ]] ||
    die "rspduo backend needs the SDRplay 3.15 library at $SDRPLAY_LIBRARY"
fi
if [[ $ENABLE_HACKRF == ON ]]; then
  pkg-config --exists libhackrf || die 'hackrf backend needs the HackRF development package'
fi
if [[ $ENABLE_USRP == ON ]]; then
  command -v uhd_config_info >/dev/null 2>&1 ||
    die 'usrp backend needs UHD 4.8 development files and uhd_config_info'
fi

if [[ $GPU == ON ]]; then
  pkg-config --exists vulkan || die 'GPU build needs Vulkan development files'
  [[ -r /usr/include/glslang/Public/ShaderLang.h || -r /usr/local/include/glslang/Public/ShaderLang.h ]] ||
    die 'GPU build needs glslang development headers'
fi

if $PREFLIGHT_ONLY; then
  say "preflight passed for backend=$BACKEND receivers=$COMPILED_RECEIVERS gpu=${GPU,,}"
  exit 0
fi
if $DRY_RUN; then
  say "dry run for backend=$BACKEND receivers=$COMPILED_RECEIVERS gpu=${GPU,,}"
fi

ensure_checkout() {
  local url=$1 directory=$2 commit=$3
  if [[ ! -d $directory/.git ]]; then
    $OFFLINE && die "offline dependency is absent: $directory"
    run git clone --filter=blob:none "$url" "$directory"
  fi
  if ! $DRY_RUN && ! git -C "$directory" cat-file -e "$commit^{commit}" 2>/dev/null; then
    $OFFLINE && die "offline dependency lacks pinned commit $commit: $directory"
    run git -C "$directory" fetch --depth 1 origin "$commit"
  fi
  run git -C "$directory" checkout --detach "$commit"
  if ! $DRY_RUN; then
    [[ $(git -C "$directory" rev-parse HEAD) == "$commit" ]] ||
      die "dependency did not resolve to pinned commit: $directory"
  fi
}

run mkdir -p "$DEPS_DIR"
ensure_checkout https://github.com/microsoft/vcpkg.git "$DEPS_DIR/vcpkg" "$VCPKG_COMMIT"
if [[ ! -x $DEPS_DIR/vcpkg/vcpkg ]]; then
  $OFFLINE && die 'vcpkg is not bootstrapped in the offline dependency cache'
  # The pinned historical wrapper uses `#!/bin/sh -e`, which some current
  # Fedora execution policies reject when invoked directly.
  # The pinned vcpkg bootstrap sources predate CMake 4. Scope CMake's official
  # compatibility floor to this wrapper only; project configuration remains
  # subject to its own policy declarations.
  run env CMAKE_POLICY_VERSION_MINIMUM=3.5 \
    /bin/sh "$DEPS_DIR/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi
if [[ $GPU != OFF ]]; then
  ensure_checkout https://github.com/DTolm/VkFFT.git "$DEPS_DIR/VkFFT" "$VKFFT_COMMIT"
fi

CMAKE_DIR="$BUILD_DIR/cmake"
MANIFEST_DIR="$BUILD_DIR/vcpkg-manifest"
ARTIFACT_TMP="$BUILD_DIR/artifact.next.$$"
ARTIFACT="$BUILD_DIR/artifact"
run mkdir -p "$BUILD_DIR"
run rm -rf "$CMAKE_DIR" "$MANIFEST_DIR" "$ARTIFACT_TMP"
run mkdir -p "$MANIFEST_DIR" "$ARTIFACT_TMP/bin"
if [[ $BACKEND == kraken ]]; then
  run cp "$SOURCE_DIR/lib/vcpkg-kraken.json" "$MANIFEST_DIR/vcpkg.json"
  KRAKEN_ONLY=ON
else
  run cp "$SOURCE_DIR/lib/vcpkg.json" "$MANIFEST_DIR/vcpkg.json"
  KRAKEN_ONLY=OFF
fi

vcpkg_cmake_prefix=()
case "$BUILD_ARCH" in
  arm*|aarch64|s390x|ppc64le|riscv*)
    # vcpkg has no downloadable helper-tool bundle for these architectures.
    # The build preflight already requires its system CMake and Ninja tools.
    vcpkg_cmake_prefix=(env VCPKG_FORCE_SYSTEM_BINARIES=1)
    ;;
esac

cmake_args=("${vcpkg_cmake_prefix[@]}" cmake -G Ninja -S "$SOURCE_DIR" -B "$CMAKE_DIR"
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
  -DBLAH2_KRAKEN_ONLY="$KRAKEN_ONLY" -DBLAH2_GPU="$GPU"
  -DBLAH2_ENABLE_RSPDUO="$ENABLE_RSPDUO"
  -DBLAH2_ENABLE_USRP="$ENABLE_USRP"
  -DBLAH2_ENABLE_HACKRF="$ENABLE_HACKRF"
  -DBLAH2_OUTPUT_DIR="$ARTIFACT_TMP/bin"
  -DCMAKE_TOOLCHAIN_FILE="$DEPS_DIR/vcpkg/scripts/buildsystems/vcpkg.cmake"
  -DVCPKG_MANIFEST_DIR="$MANIFEST_DIR"
  -DVCPKG_INSTALLED_DIR="$BUILD_DIR/vcpkg_installed")
if [[ $ENABLE_RSPDUO == ON ]]; then
  cmake_args+=("-DBLAH2_SDRPLAY_INCLUDE_DIR=$SDRPLAY_INCLUDE_DIR"
    "-DBLAH2_SDRPLAY_LIBRARY=$SDRPLAY_LIBRARY")
fi
if [[ $GPU != OFF ]]; then cmake_args+=("-DVKFFT_ROOT=$DEPS_DIR/VkFFT"); fi
run "${cmake_args[@]}"
run cmake --build "$CMAKE_DIR" --parallel "$JOBS"
run rm -f "$ARTIFACT_TMP/bin/testAcceleration" "$ARTIFACT_TMP/bin/testAccelerationFallback"

run cp -a "$SOURCE_DIR/api" "$ARTIFACT_TMP/api"
run rm -rf "$ARTIFACT_TMP/api/node_modules"
run npm ci --prefix "$ARTIFACT_TMP/api" --omit=dev --ignore-scripts --no-audit --no-fund
run find "$ARTIFACT_TMP/api" -type f -name '*.test.js' -delete
run cp -a "$SOURCE_DIR/html" "$ARTIFACT_TMP/html"
run cp -a "$SOURCE_DIR/config" "$ARTIFACT_TMP/config-examples"
run cp -a "$SOURCE_DIR/docs" "$ARTIFACT_TMP/docs"
run rm -f "$ARTIFACT_TMP/docs/GPU_ACCELERATION_WORK.md" \
  "$ARTIFACT_TMP/docs/UI_CONFIG_REVIEW.md" \
  "$ARTIFACT_TMP/docs/REAL_IQ_BENCHMARK_PLAN.md"
run cp -a "$SOURCE_DIR/contrib/systemd" "$ARTIFACT_TMP/systemd"
run mkdir -p "$ARTIFACT_TMP/licenses"
if $DRY_RUN; then
  say 'would collect vcpkg and VkFFT license notices'
else
  for dependency in asio cpp-httplib rapidjson ryml; do
    copyright_file=$(find "$BUILD_DIR/vcpkg_installed" -type f \
      -path "*/share/$dependency/copyright" -print -quit)
    [[ -n $copyright_file ]] || die "missing vcpkg license notice for $dependency"
    run install -m 0644 "$copyright_file" "$ARTIFACT_TMP/licenses/$dependency.txt"
  done
  if [[ $GPU != OFF ]]; then
    vkfft_license=
    for candidate in "$DEPS_DIR/VkFFT/LICENSE" "$DEPS_DIR/VkFFT/LICENSE.txt" "$DEPS_DIR/VkFFT/LICENSE.md"; do
      if [[ -f $candidate ]]; then vkfft_license=$candidate; break; fi
    done
    [[ -n $vkfft_license ]] || die 'missing VkFFT license notice'
    run install -m 0644 "$vkfft_license" "$ARTIFACT_TMP/licenses/VkFFT.txt"
  fi
fi
run mkdir -p "$ARTIFACT_TMP/libexec"
run install -m 0755 "$SOURCE_DIR/script/vectorwarp-restart" "$ARTIFACT_TMP/libexec/vectorwarp-restart"
run install -m 0755 "$SOURCE_DIR/script/vectorwarp-wait-api.js" "$ARTIFACT_TMP/libexec/vectorwarp-wait-api.js"
run install -m 0644 "$SOURCE_DIR/LICENSE" "$ARTIFACT_TMP/LICENSE"
run install -m 0644 "$SOURCE_DIR/README.md" "$ARTIFACT_TMP/README.md"

if ! $DRY_RUN; then
  [[ -x $ARTIFACT_TMP/bin/blah2 ]] || die 'processor binary was not produced'
  [[ -f $ARTIFACT_TMP/api/server.js && -f $ARTIFACT_TMP/html/index.html ]] ||
    die 'API/UI artifact is incomplete'
  revision=$(git -C "$SOURCE_DIR" rev-parse --short=12 HEAD 2>/dev/null || printf unknown)
  build_id="$(date -u +%Y%m%dT%H%M%SZ)-$revision"
  build_os_id=unknown
  build_os_version=unknown
  if [[ -r /etc/os-release ]]; then
    build_os_id=$(sed -n 's/^ID=//p' /etc/os-release | tr -d '"' | head -n 1)
    build_os_version=$(sed -n 's/^VERSION_ID=//p' /etc/os-release | tr -d '"' | head -n 1)
  fi
  printf 'build_id=%s\nbackend=%s\ncompiled_receivers=%s\ngpu=%s\nbuild_os_id=%s\nbuild_os_version=%s\nbuild_arch=%s\nvcpkg_commit=%s\nvkfft_commit=%s\n' \
    "$build_id" "$BACKEND" "$COMPILED_RECEIVERS" "$GPU" "$build_os_id" "$build_os_version" "$BUILD_ARCH" \
    "$VCPKG_COMMIT" "$VKFFT_COMMIT" >"$ARTIFACT_TMP/.vectorwarp-build"
  rm -rf "$ARTIFACT"
  mv "$ARTIFACT_TMP" "$ARTIFACT"
  say "artifact ready: $ARTIFACT"
  say "review it, then install separately with script/install-native.sh"
fi
