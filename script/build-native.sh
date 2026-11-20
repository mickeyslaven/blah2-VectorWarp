#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
BUILD_DIR="$SOURCE_DIR/build/native"
DEPS_DIR="$SOURCE_DIR/build/native-deps"
BACKEND=all
GPU=AUTO
JOBS=4
OFFLINE=false
DRY_RUN=false
PREFLIGHT_ONLY=false
RUN_TESTS=false
SDRPLAY_INCLUDE_DIR=${BLAH2_SDRPLAY_INCLUDE_DIR:-/usr/local/include}
SDRPLAY_LIBRARY=${BLAH2_SDRPLAY_LIBRARY:-/usr/local/lib/libsdrplay_api.so.3.15}

ARTIFACT_PLAN="$SOURCE_DIR/script/artifact-plan.py"
VCPKG_COMMIT=$(python3 "$ARTIFACT_PLAN" pin vcpkg)
VKFFT_COMMIT=$(python3 "$ARTIFACT_PLAN" pin VkFFT)

usage() {
  cat <<'EOF'
Usage: script/build-native.sh [options]

Build a relocatable VectorWarp artifact without installing it.

  --backend all           Unified build (default); other values are internal fixtures only
  --gpu auto|on|off       Optional Vulkan worker selection (default: auto)
  --build-dir PATH        Build/output directory (default: build/native)
  --deps-dir PATH         Pinned source dependency cache
  --jobs N                Parallel build jobs (default: 4)
  --offline               Never fetch; require both pinned checkouts locally
  --preflight             Check tools and host SDKs, then stop
  --test                  Build and run the native CTest suite
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
    --test) RUN_TESTS=true; shift ;;
    --dry-run) DRY_RUN=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

if [[ $BACKEND != all && ${VECTORWARP_INTERNAL_ADAPTER_FIXTURE:-false} != true ]]; then
  die 'receiver-subset builds are internal adapter fixtures; use --backend all'
fi
profile=$(python3 "$ARTIFACT_PLAN" profile "$BACKEND") || exit 1
read -r ENABLE_RSPDUO ENABLE_USRP ENABLE_HACKRF LOCAL_BUILD_RSPDUO COMPILED_RECEIVERS LOCAL_BUILD_RECEIVERS TEST_ONLY <<<"$profile"
[[ $LOCAL_BUILD_RECEIVERS != - ]] || LOCAL_BUILD_RECEIVERS=
[[ $GPU =~ ^(AUTO|ON|OFF)$ ]] || die '--gpu must be auto, on or off'
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || die '--jobs must be a positive integer'
BUILD_DIR=$(realpath -m "$BUILD_DIR")
DEPS_DIR=$(realpath -m "$DEPS_DIR")
BUILD_ARCH=$(uname -m)
[[ $BUILD_DIR != / && $DEPS_DIR != / ]] || die 'refusing to use / as a work directory'
[[ $BUILD_DIR != "$SOURCE_DIR" && $DEPS_DIR != "$SOURCE_DIR" ]] ||
  die 'build and dependency directories must not replace the source tree'

for command in cmake git node npm curl tar zip unzip pkg-config cc c++ ninja python3; do need_command "$command"; done
node "$SOURCE_DIR/script/check-node-version.js" || die 'a patched supported Node.js release is required'
for file in CMakeLists.txt lib/vcpkg.json api/package.json html/index.html; do
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
  # Some native libhackrf .pc files expose libusb headers without declaring the
  # development package dependency. Fail preflight before a long vcpkg build.
  pkg-config --exists libusb-1.0 ||
    die 'hackrf backend needs libusb development files (libusb-1.0-0-dev or libusb1-devel)'
fi
if [[ $ENABLE_USRP == ON ]]; then
  command -v uhd_config_info >/dev/null 2>&1 ||
    die 'usrp backend needs UHD development files and uhd_config_info'
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

# A development build may use local edits, but it must never claim that its
# bytes came from an immutable release commit. Capture this before building as
# well as afterwards; a commit made during compilation also invalidates it.
source_clean=false
if ! $DRY_RUN; then
  source_commit=$(git -C "$SOURCE_DIR" rev-parse HEAD)
  [[ $source_commit =~ ^[0-9a-f]{40}$ ]] || die 'cannot identify source commit'
  if [[ -z $(git -C "$SOURCE_DIR" status --porcelain --untracked-files=normal) ]]; then
    source_clean=true
  fi
fi

dependency_options=()
if $OFFLINE; then dependency_options+=(--offline); fi
if $DRY_RUN; then dependency_options+=(--dry-run); fi

run mkdir -p "$DEPS_DIR"
python3 "$ARTIFACT_PLAN" checkout vcpkg "$DEPS_DIR/vcpkg" "${dependency_options[@]}"
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
  python3 "$ARTIFACT_PLAN" checkout VkFFT "$DEPS_DIR/VkFFT" "${dependency_options[@]}"
fi

CMAKE_DIR="$BUILD_DIR/cmake"
TEST_CMAKE_DIR="$BUILD_DIR/cmake-tests"
TEST_OUTPUT_DIR="$BUILD_DIR/test-bin"
MANIFEST_DIR="$BUILD_DIR/vcpkg-manifest"
ARTIFACT_TMP="$BUILD_DIR/artifact.next.$$"
ARTIFACT="$BUILD_DIR/artifact"
run mkdir -p "$BUILD_DIR"
run rm -rf "$CMAKE_DIR" "$TEST_CMAKE_DIR" "$TEST_OUTPUT_DIR" "$MANIFEST_DIR" "$ARTIFACT_TMP"
run mkdir -p "$MANIFEST_DIR" "$ARTIFACT_TMP/bin"
run cp "$SOURCE_DIR/lib/vcpkg.json" "$MANIFEST_DIR/vcpkg.json"
KRAKEN_ONLY=OFF
if [[ $BACKEND == kraken ]]; then KRAKEN_ONLY=ON; fi

vcpkg_tool_path=$PATH
vcpkg_compiler_prefix=
case "$BUILD_ARCH" in
  aarch64|arm64)
    vcpkg_compiler_prefix=aarch64-linux-gnu
    compiler_machine_pattern='^(aarch64|arm64)'
    ;;
  arm*)
    vcpkg_compiler_prefix=arm-linux-gnueabihf
    compiler_machine_pattern='^arm'
    ;;
esac
if [[ -n $vcpkg_compiler_prefix ]]; then
  native_cc=$(command -v cc)
  native_cxx=$(command -v c++)
  native_cc_machine=$("$native_cc" -dumpmachine)
  native_cxx_machine=$("$native_cxx" -dumpmachine)
  [[ $native_cc_machine =~ $compiler_machine_pattern ]] ||
    die "native C compiler target $native_cc_machine does not match host $BUILD_ARCH"
  [[ $native_cxx_machine =~ $compiler_machine_pattern ]] ||
    die "native C++ compiler target $native_cxx_machine does not match host $BUILD_ARCH"
  # Pinned vcpkg's Linux ARM toolchain unconditionally requests Debian-style
  # compiler names, which native Fedora does not provide. Supply verified
  # aliases only inside the freshly recreated build tree and only to vcpkg.
  compiler_alias_dir="$BUILD_DIR/native-compiler-aliases"
  run rm -rf "$compiler_alias_dir"
  run mkdir -p "$compiler_alias_dir"
  run ln -s "$native_cc" "$compiler_alias_dir/$vcpkg_compiler_prefix-gcc"
  run ln -s "$native_cxx" "$compiler_alias_dir/$vcpkg_compiler_prefix-g++"
  vcpkg_tool_path="$compiler_alias_dir:$PATH"
fi

# RapidJSON's fixed 2023 snapshot still declares a pre-3.5 CMake floor. CMake
# 4 removed that compatibility mode, so scope its official policy floor to the
# configure process that performs the pinned vcpkg manifest install. The blah2
# project declares its own newer minimum and is unaffected.
vcpkg_cmake_prefix=(env CMAKE_POLICY_VERSION_MINIMUM=3.5)
case "$BUILD_ARCH" in
  arm*|aarch64|s390x|ppc64le|riscv*)
    # vcpkg has no downloadable helper-tool bundle for these architectures.
    # The build preflight already requires its system CMake and Ninja tools.
    vcpkg_cmake_prefix+=(VCPKG_FORCE_SYSTEM_BINARIES=1)
    ;;
esac
if [[ $vcpkg_tool_path != "$PATH" ]]; then
  vcpkg_cmake_prefix+=("PATH=$vcpkg_tool_path")
fi

cmake_args=(
  -DCMAKE_BUILD_TYPE=Release
  -DBLAH2_KRAKEN_ONLY="$KRAKEN_ONLY" -DBLAH2_GPU="$GPU"
  -DBLAH2_ENABLE_RSPDUO="$ENABLE_RSPDUO"
  -DBLAH2_LOCAL_BUILD_RSPDUO="$LOCAL_BUILD_RSPDUO"
  -DBLAH2_ENABLE_USRP="$ENABLE_USRP"
  -DBLAH2_ENABLE_HACKRF="$ENABLE_HACKRF"
  -DCMAKE_TOOLCHAIN_FILE="$DEPS_DIR/vcpkg/scripts/buildsystems/vcpkg.cmake"
  -DVCPKG_MANIFEST_DIR="$MANIFEST_DIR"
  -DVCPKG_INSTALLED_DIR="$BUILD_DIR/vcpkg_installed")
if [[ $ENABLE_RSPDUO == ON ]]; then
  cmake_args+=("-DBLAH2_SDRPLAY_INCLUDE_DIR=$SDRPLAY_INCLUDE_DIR"
    "-DBLAH2_SDRPLAY_LIBRARY=$SDRPLAY_LIBRARY")
fi
if [[ $GPU != OFF ]]; then cmake_args+=("-DVKFFT_ROOT=$DEPS_DIR/VkFFT"); fi
if $RUN_TESTS; then
  # Test executables retain build-tree RUNPATHs, so keep them outside the
  # immutable release artifact. The production configure is always test-free.
  run "${vcpkg_cmake_prefix[@]}" cmake -G Ninja -S "$SOURCE_DIR" -B "$TEST_CMAKE_DIR" \
    "${cmake_args[@]}" -DBUILD_TESTING=ON -DBLAH2_OUTPUT_DIR="$TEST_OUTPUT_DIR"
  run cmake --build "$TEST_CMAKE_DIR" --parallel "$JOBS"
  run ctest --test-dir "$TEST_CMAKE_DIR" --output-on-failure --no-tests=error
fi
run "${vcpkg_cmake_prefix[@]}" cmake -G Ninja -S "$SOURCE_DIR" -B "$CMAKE_DIR" \
  "${cmake_args[@]}" -DBUILD_TESTING=OFF -DBLAH2_OUTPUT_DIR="$ARTIFACT_TMP/bin"
run cmake --build "$CMAKE_DIR" --parallel "$JOBS"
run python3 "$SOURCE_DIR/script/build-kraken.py" \
  --build-dir "$BUILD_DIR/kraken-native" \
  --jobs "$((JOBS > 32 ? 32 : JOBS))" --output-dir "$ARTIFACT_TMP"
run install -m 0644 "$SOURCE_DIR/script/vectorwarp-kraken-macos.py" "$ARTIFACT_TMP/share/heimdall/controller.py"
run install -m 0644 "$SOURCE_DIR/script/vectorwarp-heimdall.py" "$ARTIFACT_TMP/share/heimdall/service.py"
run python3 "$SOURCE_DIR/script/stage-kraken-kit.py" --source "$SOURCE_DIR" \
  --output "$ARTIFACT_TMP/receiver-source/kraken"

run python3 "$ARTIFACT_PLAN" stage --source "$SOURCE_DIR" --output "$ARTIFACT_TMP" --platform linux
run npm ci --prefix "$ARTIFACT_TMP/api" --omit=dev --ignore-scripts --no-audit --no-fund
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
if [[ $LOCAL_BUILD_RSPDUO == ON && $DRY_RUN == false ]]; then
  [[ -f $CMAKE_DIR/receiver-generated/rspduo-plan.json ]] || die 'local RSPduo kit plan was not generated'
  run python3 "$SOURCE_DIR/script/stage-rspduo-kit.py" --source "$SOURCE_DIR" \
    --generated "$CMAKE_DIR/receiver-generated" --core "$ARTIFACT_TMP/bin/libblah2-capture-core.so.1" \
    --output "$ARTIFACT_TMP/receiver-source/rspduo"
fi
if ! $DRY_RUN; then
  python3 "$ARTIFACT_PLAN" validate "$ARTIFACT_TMP" --level build --backend "$BACKEND"
  [[ $(git -C "$SOURCE_DIR" rev-parse HEAD) == "$source_commit" ]] ||
    die 'source commit changed during build; rebuild the frozen candidate'
  [[ -z $(git -C "$SOURCE_DIR" status --porcelain --untracked-files=normal) ]] || source_clean=false
  revision=${source_commit:0:12}
  build_id="$(date -u +%Y%m%dT%H%M%SZ)-$revision"
  build_os_id=unknown
  build_os_version=unknown
  if [[ -r /etc/os-release ]]; then
    build_os_id=$(sed -n 's/^ID=//p' /etc/os-release | tr -d '"' | head -n 1)
    build_os_version=$(sed -n 's/^VERSION_ID=//p' /etc/os-release | tr -d '"' | head -n 1)
  fi
  printf 'build_id=%s\nsource_commit=%s\nsource_clean=%s\nbackend=%s\ncompiled_receivers=%s\nlocal_build_receivers=%s\ntest_only=%s\ngpu=%s\nbuild_os_id=%s\nbuild_os_version=%s\nbuild_arch=%s\nvcpkg_commit=%s\nvkfft_commit=%s\n' \
    "$build_id" "$source_commit" "$source_clean" "$BACKEND" "$COMPILED_RECEIVERS" "$LOCAL_BUILD_RECEIVERS" "$TEST_ONLY" "$GPU" "$build_os_id" "$build_os_version" "$BUILD_ARCH" \
    "$VCPKG_COMMIT" "$VKFFT_COMMIT" >"$ARTIFACT_TMP/.vectorwarp-build"
  rm -rf "$ARTIFACT"
  mv "$ARTIFACT_TMP" "$ARTIFACT"
  say "artifact ready: $ARTIFACT"
  say "review it, then install separately with script/install-native.sh"
fi
