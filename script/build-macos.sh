#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
ARTIFACT_PLAN="$SOURCE_DIR/script/artifact-plan.py"
BUILD_DIR="$SOURCE_DIR/build/macos"
OUTPUT_DIR=""
BACKEND=all
WITH_RSPDUO=false
JOBS=4
RUN_TESTS=false
PHYSICAL_GPU_TESTS=OFF
GPU=auto

usage() {
  cat <<'EOF'
Usage: script/build-macos.sh [options]

Build a native macOS artifact. GPU acceleration defaults to automatic and
qualifies each available stage independently at runtime.

  --backend all                       Unified build (default); subsets are internal fixtures only
  --with-rspduo                     Also compile RSPduo against explicit local SDK paths
  --build-dir PATH                   CMake build root (default: build/macos)
  --output-dir PATH                  Artifact root (default: BUILD_DIR/artifact)
  --jobs N                           Parallel build jobs (default: 4)
  --test                             Run CTest after a successful build
  --physical-gpu-test                Also require physical GPU CTests (implies --test)
  --gpu off|auto|on                  Optional macOS GPU build (default: auto)
  -h, --help                         Show this help

`all` builds the open USRP and HackRF adapters and the local RSPduo SDK kit.
`--with-rspduo` additionally compiles that adapter using caller-supplied
`BLAH2_SDRPLAY_INCLUDE_DIR` and `BLAH2_SDRPLAY_LIBRARY` values; this script
never downloads vendor files or contacts an SDR. Partial builds are internal
test fixtures, not installable product variants.
EOF
}

die() { printf 'build-macos: %s\n' "$*" >&2; exit 1; }
while (($#)); do
  case "$1" in
    --backend) (($# >= 2)) || die '--backend needs a value'; BACKEND=$2; shift 2 ;;
    --with-rspduo) WITH_RSPDUO=true; shift ;;
    --build-dir) (($# >= 2)) || die '--build-dir needs a path'; BUILD_DIR=$2; shift 2 ;;
    --output-dir) (($# >= 2)) || die '--output-dir needs a path'; OUTPUT_DIR=$2; shift 2 ;;
    --jobs) (($# >= 2)) || die '--jobs needs a value'; JOBS=$2; shift 2 ;;
    --test) RUN_TESTS=true; shift ;;
    --physical-gpu-test) RUN_TESTS=true; PHYSICAL_GPU_TESTS=ON; shift ;;
    --gpu) (($# >= 2)) || die '--gpu needs off, auto, or on'; GPU=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

normalize_path() { python3 -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$1"; }

[[ $(uname -s) == Darwin ]] || die 'this script runs on macOS only'
[[ $BACKEND =~ ^(cpu|kraken|usrp|hackrf|rspduo|all)$ ]] || die 'backend must be cpu, kraken, usrp, hackrf, rspduo, or all'
if [[ $BACKEND != all && ${VECTORWARP_INTERNAL_ADAPTER_FIXTURE:-false} != true ]]; then
  die 'receiver-subset builds are internal adapter fixtures; use --backend all'
fi
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || die 'jobs must be a positive integer'
[[ $GPU =~ ^(off|auto|on)$ ]] || die '--gpu must be off, auto, or on'
[[ $PHYSICAL_GPU_TESTS != ON || $GPU != off ]] || die '--physical-gpu-test requires GPU support'
if [[ $GPU == on ]]; then
  [[ ${VKFFT_ROOT:-} == /* && -d ${VKFFT_ROOT:-} ]] ||
    die '--gpu on needs VKFFT_ROOT set to an absolute local VkFFT directory'
fi
if command -v brew >/dev/null; then
  BREW_PREFIX=$(brew --prefix)
elif [[ ${VECTORWARP_MACOS_BREW_PREFIX:-} == /* && -d ${VECTORWARP_MACOS_BREW_PREFIX:-} ]]; then
  # Formula builds execute in Homebrew's restricted build environment, where
  # the `brew` command is intentionally absent. Dependencies remain under the
  # supplied Homebrew prefix and are verified below.
  BREW_PREFIX=$VECTORWARP_MACOS_BREW_PREFIX
else
  die 'Homebrew is required; install it, then rerun'
fi
for command in cmake ninja node npm python3; do command -v "$command" >/dev/null || die "missing $command; install it, then rerun"; done
node "$SOURCE_DIR/script/check-node-version.js" || die 'a patched supported Node.js release is required'

BUILD_DIR=$(normalize_path "$BUILD_DIR")
[[ -n $OUTPUT_DIR ]] || OUTPUT_DIR="$BUILD_DIR/artifact"
OUTPUT_DIR=$(normalize_path "$OUTPUT_DIR")
for protected in "$SOURCE_DIR" "$SOURCE_DIR/api" "$SOURCE_DIR/html" "$SOURCE_DIR/config" "$SOURCE_DIR/script"; do
  [[ $OUTPUT_DIR == "$protected" || $BUILD_DIR == "$protected" ]] &&
    die 'build and output paths must not replace source-tree components'
done
[[ $OUTPUT_DIR != "$BUILD_DIR" ]] || die 'output directory must be separate from build directory'
[[ ! -e $OUTPUT_DIR/bin/blah2-mixed-worker && ! -L $OUTPUT_DIR/bin/blah2-mixed-worker ]] ||
  die 'historical mixed DSP workers are not part of the unified build artifact; choose a clean output directory'

have_formula() {
  if command -v brew >/dev/null; then
    brew list --versions "$1" >/dev/null 2>&1
  else
    [[ -d "$BREW_PREFIX/opt/$1" ]]
  fi
}
dependencies=(asio rapidyaml cpp-httplib armadillo fftw rapidjson)
if $RUN_TESTS; then dependencies+=(catch2); fi
for formula in "${dependencies[@]}"; do
  have_formula "$formula" ||
    die "missing $formula; run: brew install $formula"
done

# cpu is an existing macOS internal no-device fixture, equivalent to Kraken-only.
profile_backend=$BACKEND; [[ $profile_backend != cpu ]] || profile_backend=kraken
profile_options=(); if $WITH_RSPDUO; then profile_options+=(--with-rspduo); fi
profile=$(python3 "$ARTIFACT_PLAN" profile "$profile_backend" "${profile_options[@]}") || exit 1
read -r ENABLE_RSPDUO ENABLE_USRP ENABLE_HACKRF LOCAL_RSPDUO unused <<<"$profile"
UHD_FORMULA=${BLAH2_UHD_FORMULA:-uhd}
for formula in $([[ $ENABLE_USRP == ON ]] && printf '%s' "$UHD_FORMULA") $([[ $ENABLE_HACKRF == ON ]] && printf 'hackrf'); do
  have_formula "$formula" ||
    die "missing $formula SDK; run: brew install $formula"
done
if [[ $ENABLE_RSPDUO == ON ]]; then
  [[ ${BLAH2_SDRPLAY_INCLUDE_DIR:-} == /* ]] ||
    die 'rspduo needs an absolute BLAH2_SDRPLAY_INCLUDE_DIR'
  [[ ${BLAH2_SDRPLAY_LIBRARY:-} == /* ]] ||
    die 'rspduo needs an absolute BLAH2_SDRPLAY_LIBRARY'
  [[ -n ${BLAH2_SDRPLAY_INCLUDE_DIR:-} && -f ${BLAH2_SDRPLAY_INCLUDE_DIR}/sdrplay_api.h ]] ||
    die 'rspduo needs BLAH2_SDRPLAY_INCLUDE_DIR containing sdrplay_api.h'
  [[ -n ${BLAH2_SDRPLAY_LIBRARY:-} && -f ${BLAH2_SDRPLAY_LIBRARY} ]] ||
    die 'rspduo needs BLAH2_SDRPLAY_LIBRARY naming the supplied SDK runtime library'
fi

CMAKE_DIR="$BUILD_DIR/cmake"
case "$GPU" in
  off) GPU_CMAKE=OFF ;;
  auto) GPU_CMAKE=AUTO ;;
  on) GPU_CMAKE=ON ;;
esac
# Hosted macOS VMs can compile GPU modules without exposing physical hardware.
# Only the explicit --physical-gpu-test option enables hardware qualification.
cmake -G Ninja -S "$SOURCE_DIR" -B "$CMAKE_DIR" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING="$RUN_TESTS" \
  -DBLAH2_TEST_PHYSICAL_GPU="$PHYSICAL_GPU_TESTS" \
  -DBLAH2_KRAKEN_ONLY=OFF -DBLAH2_ENABLE_USRP="$ENABLE_USRP" \
  -DBLAH2_ENABLE_HACKRF="$ENABLE_HACKRF" -DBLAH2_ENABLE_RSPDUO="$ENABLE_RSPDUO" \
  -DBLAH2_LOCAL_BUILD_RSPDUO="$LOCAL_RSPDUO" \
  -DBLAH2_SDRPLAY_INCLUDE_DIR="${BLAH2_SDRPLAY_INCLUDE_DIR:-/usr/local/include}" \
  -DBLAH2_SDRPLAY_LIBRARY="${BLAH2_SDRPLAY_LIBRARY:-/usr/local/lib/libsdrplay_api.so.3.15}" \
  -DBLAH2_GPU="$GPU_CMAKE" -DVKFFT_ROOT="${VKFFT_ROOT:-}" -DBLAH2_OUTPUT_DIR="$OUTPUT_DIR/bin" \
  -DCMAKE_PREFIX_PATH="$BREW_PREFIX;$([[ $ENABLE_USRP == ON ]] && brew --prefix "$UHD_FORMULA")"
cmake --build "$CMAKE_DIR" --parallel "$JOBS"
if $RUN_TESTS; then ctest --test-dir "$CMAKE_DIR" --output-on-failure; fi
find "$OUTPUT_DIR/bin" -maxdepth 1 -type f -name 'test*' -delete
rm -rf "$OUTPUT_DIR/bin/test"
python3 "$ARTIFACT_PLAN" stage --source "$SOURCE_DIR" --output "$OUTPUT_DIR" --platform macos
rm -f "$OUTPUT_DIR/vectorwarp-macos"
if [[ $LOCAL_RSPDUO == ON ]]; then
  kit_parent="$OUTPUT_DIR/receiver-source"
  mkdir -p "$kit_parent"
  kit_next="$(mktemp -d "$kit_parent/.rspduo.next.XXXXXX")"
  kit_previous="$kit_next/previous"
  # The stager deliberately refuses an existing destination.  Build the whole
  # kit in a fresh, owned sibling so a failed stage leaves the last usable kit.
  if ! python3 "$SOURCE_DIR/script/stage-rspduo-kit.py" --source "$SOURCE_DIR" \
    --generated "$CMAKE_DIR/receiver-generated" --core "$OUTPUT_DIR/bin/libblah2-capture-core.dylib" \
    --output "$kit_next/rspduo"; then
    rm -rf "$kit_next"
    die 'could not stage the RSPduo receiver source kit'
  fi
  if [[ -e "$kit_parent/rspduo" || -L "$kit_parent/rspduo" ]]; then
    mv "$kit_parent/rspduo" "$kit_previous"
  fi
  if ! mv "$kit_next/rspduo" "$kit_parent/rspduo"; then
    if [[ -e "$kit_previous" || -L "$kit_previous" ]] &&
        ! mv "$kit_previous" "$kit_parent/rspduo"; then
      die "could not publish the RSPduo receiver source kit; previous kit remains at $kit_previous"
    fi
    rm -rf "$kit_next"
    die 'could not publish the RSPduo receiver source kit'
  fi
  rm -rf "$kit_previous"
  rmdir "$kit_next"
  install -m 0755 "$SOURCE_DIR/script/vectorwarp-build-sdrplay-macos.sh" "$OUTPUT_DIR/script/vectorwarp-build-sdrplay-macos.sh"
  install -m 0644 "$SOURCE_DIR/script/vectorwarp-build-sdrplay-macos.js" "$OUTPUT_DIR/script/vectorwarp-build-sdrplay-macos.js"
fi
# npm 11 resolves a prefixed project through a different code path.  Running
# from the staged directory keeps package and lockfile discovery local to the
# artifact, including when its root is an absolute path.
(
  cd "$OUTPUT_DIR/api"
  npm ci --omit=dev --ignore-scripts >/dev/null
)
printf 'build-macos: artifact root: %s\n' "$OUTPUT_DIR"
