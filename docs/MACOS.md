# macOS installation and local development

This port is under local validation. Public Homebrew formulas follow successful
[main-merge publishing runs](HOMEBREW_PUBLISHING.md); the local source
installation is available now. Linux release installation and systemd
services remain separate. Apple Silicon
(`arm64`) was exercised locally on an Apple M2 running macOS 26.6.1. That work
covered CPU and replay processing, browser configuration and service lifecycle,
the local Homebrew app and Kraken companion through revision 17, a calibrated
local USB Kraken, and MoltenVK ambiguity/clutter processing. The
[macOS CI run](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35400739948)
also passed all source checks and an installed Homebrew app/Heimdall lifecycle
and synthetic-pipeline check on `macos-15`. Intel (`x86_64`)
is an experimental source-level CI target. Its `macos-15-intel` job passed CPU,
open receiver-adapter, synthetic Kraken, replay, API, browser and lifecycle
checks. Physical Intel receivers, GPU processing and installed Homebrew remain
unverified.

The separate universal standalone `.pkg` is Developer ID signed, Apple-notarized
and installs at `/Applications/VectorWarp.app`. Published versions appear in the
[release download matrix](https://mickeyslaven.github.io/blah2-VectorWarp/#install).
See [MACOS_STANDALONE.md](MACOS_STANDALONE.md) for its target
layout, commands and qualification boundaries.
Homebrew is another macOS packaging path: see [MACOS_HOMEBREW.md](MACOS_HOMEBREW.md)
for a local source snapshot, service lifecycle, upgrades and removal. CPU processing
is always available. Optional Vulkan/MoltenVK processing is described in
[MACOS_GPU.md](MACOS_GPU.md); readiness comes from numerical checks on the running
processor, independently for each stage.

## Prerequisites

Use Xcode Command Line Tools, Python 3.9 or newer, and Node.js 24 LTS. Install the open-source
build dependencies with Homebrew:

```sh
brew install node@24 cmake ninja pkgconf asio rapidyaml cpp-httplib armadillo fftw rapidjson catch2
```

For the USRP/B210 and dual-HackRF adapters, install the open receiver libraries:

```sh
brew install uhd hackrf
```

VectorWarp does not download receiver firmware or the proprietary SDRplay SDK.
Get the macOS API 3.15 and headers directly from
[SDRplay](https://sdrplay.com/hardware-api/) if you want RSPduo support. Follow
its installation and API-service instructions locally. A separately installed
SDK alone does not add an adapter. The Homebrew package includes a checked
local adapter source kit; Settings can compile that adapter against your manual
SDK installation. A direct source build can instead enable the RSPduo option.

## Build and run

From the repository root:

```sh
script/build-macos.sh --backend all --test
build/macos/artifact/script/vectorwarp-macos
```

The default `all` profile builds USRP and HackRF adapters using the installed
open-source libraries. `--backend cpu` builds CPU/replay without those SDKs.
`--backend usrp` and `--backend hackrf` select one optional adapter. Kraken's
existing built-in network client remains compiled; these commands do not
configure or connect to a Kraken host. Native local USB Kraken support uses
the separately built Heimdall companion; see [MACOS_KRAKEN.md](MACOS_KRAKEN.md).
The Homebrew formula installs that companion automatically.

For RSPduo, install the vendor SDK first, then explicitly build against its local
paths (adjust the paths to your installation):

```sh
BLAH2_SDRPLAY_INCLUDE_DIR=/usr/local/include \
BLAH2_SDRPLAY_LIBRARY=/usr/local/lib/libsdrplay_api.so.3.15 \
  script/build-macos.sh --backend rspduo --build-dir build/macos-rspduo --test
```

To include USRP, HackRF and RSPduo together, use the same explicit SDK variables
with `--backend all --with-rspduo --build-dir build/macos-all-sdr --test`.
The default `all` still includes only the open-source receiver libraries.

This compiles VectorWarp's adapter locally; it does not copy or package the
vendor SDK. The macOS source-kit workflow uses the current user’s writable
application-support directory and AppleClang. The Linux privileged builder stays
separate. Physical capture still requires hardware validation.

Use the staged launcher for explicit lifecycle actions:

```sh
build/macos/artifact/script/vectorwarp-macos start
build/macos/artifact/script/vectorwarp-macos status
build/macos/artifact/script/vectorwarp-macos restart
build/macos/artifact/script/vectorwarp-macos stop
build/macos/artifact/script/vectorwarp-macos logs
```

The staged tree uses this Mac's Homebrew libraries and Node installation; it
is not a self-contained distributable `.app`, DMG, universal binary, or signed
installer. Move the whole artifact together, retaining those dependencies.

## Runtime scope

The macOS launcher is per-user and starts only the browser interface by default.
The launcher alone does not register a login service. Homebrew’s explicit
`brew services start vectorwarp` registers a per-user LaunchAgent;
`brew services stop vectorwarp` stops and unregisters it. Start processing explicitly after
choosing a valid receiver or replay file in Settings.

For installed Homebrew command names, updates, and service restarts, see the
[macOS Homebrew guide](MACOS_HOMEBREW.md#public-tap-updates).
The local development tap must be regenerated from its source snapshot; it does
not receive public-tap upgrades.

The default state directory is `~/Library/Application Support/VectorWarp`.
The initial config binds to `127.0.0.1`. State, configuration, recordings and
logs belong in this user's writable application-support directory.
For isolated development or tests, set `VECTORWARP_MACOS_STATE` and optionally
`VECTORWARP_MACOS_CONFIG`, `VECTORWARP_MACOS_PROCESSOR` and
`VECTORWARP_MACOS_NODE`. Keep the selected config/processor consistent for all
commands against the same state directory. SDRplay's vendor service is independently
managed. The launcher owns only its own local Kraken controller and never stops
another application's receiver service.

Local USB Kraken is the live receiver path exercised on the M2. USRP/B210 and
dual HackRF use actual locally linked SDK modules but only simulated capture in
this work; no attached hardware result is claimed. The optional RSPduo adapter
uses a manually installed SDK and controlled missing-device/replay checks, not
a physical RSPduo. No remote Kraken setup is part of this port's acceptance
work.

## Validation boundary

Synthetic recordings, SDK stubs, loopback fixtures and library-load checks are
software tests. They do not prove physical USB capture, receiver tuning,
clocking, coherence, sustained sample throughput or RF behavior. The M2 Kraken
run proves a bounded local USB/capture/calibration path, not external RF
coherence, arbitrary interruption recovery, or a hard real-time deadline.
Apple Silicon and Intel macOS need separate execution evidence. The presence of
a Vulkan SDK or an Apple GPU does not qualify GPU processing; CPU fallback stays
available.

See [MACOS_COMPATIBILITY.md](MACOS_COMPATIBILITY.md) for vendor sources and the
initial platform audit. Build commands and measured acceptance results below
are updated as local validation completes.

## Repeatable software checks

```sh
npm ci --prefix api --ignore-scripts --no-audit --no-fund
export NODE_PATH="$PWD/api/node_modules"
npm test --prefix api
npm run test:dom --prefix api
script/vectorwarp-macos.test.sh
python3 test/recording/processor_replay_test.py --binary build/macos/artifact/bin/blah2
python3 test/macos/processor_lifecycle_test.py --binary build/macos/artifact/bin/blah2
python3 test/macos/config_runtime_test.py --binary build/macos/artifact/bin/blah2
python3 test/macos/receiver_sdk_test.py --binary build/macos/artifact/bin/blah2
```

`--test` runs native CTest before staging, including macOS GPU worker
isolation fixtures that do not open a GPU. Linux privileged receiver-helper
tests require Linux; the Linux CI/package matrix retains that coverage. The
macOS workflow has passed its Apple Silicon and Intel runner jobs, providing
source-level CPU, open adapter, synthetic Kraken, replay, API, browser and
lifecycle evidence on both architectures. It does not qualify Intel hardware,
GPU processing, or an installed Homebrew package.

The matching [Linux release-package run](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35400739837)
passed all ten OS/architecture jobs, including their installed-service and
Chromium smoke checks. Those results do not replace hardware qualification.

The launcher uses Python’s kernel file locking and macOS process identity APIs.
It records exact arguments and process birth time to avoid stopping an unrelated
process when a PID is reused.

For separately compiled RSPduo integration tests, the SDK harness accepts
`--include-rspduo --sdrplay-service /absolute/path/to/sdrplay_apiService` with a
binary containing all three optional adapters. This starts the explicitly
supplied service temporarily, tests nonexistent receiver identities and replay,
then stops only that service process. It does not install the SDK or register a
daemon. Keep vendor files outside the checkout and staged artifact.

For the normal local-source-kit package, also pass `--local-rspduo-kit` and
explicitly set `VECTORWARP_MACOS_STATE` to a private test directory under
`~/Library/Application Support/VectorWarp`, plus `BLAH2_SDRPLAY_INCLUDE_DIR`
and `BLAH2_SDRPLAY_LIBRARY` to the manually obtained SDK. This compiles the
staged kit, checks API receipt status and native loading, then runs the same
real SDK missing-device and synthetic replay checks. It accepts normal SDK
symlinks and does not copy vendor files into the package.

An optional six-minute supervisor check is available with
`processor_lifecycle_test.py --binary PATH --endurance-seconds 360`. It tests
actual API/processor crash recovery, process-pause staleness and replay memory
growth. Physical GPU application checks require a GPU-enabled artifact:
`python3 test/macos/gpu_runtime_test.py /absolute/path/to/bin/blah2`.

Measured local results and the configuration coverage boundary are recorded in
[MACOS_TEST_MATRIX.md](MACOS_TEST_MATRIX.md).

## Local ADS-B feeds

Settings uses the same decoder choices on both platforms. On macOS, local
discovery checks `<brew-prefix>/var/run/<decoder>/aircraft.json` and
`~/Library/Application Support/VectorWarp/adsb/<decoder>/aircraft.json`.
The standard Homebrew prefix is `/opt/homebrew` on Apple Silicon and
`/usr/local` on Intel; an explicit `HOMEBREW_PREFIX` can override it. Decoder
names are `readsb`, `dump1090-fa`, `dump1090`, and `dump1090-mutability`.
Configure your separately installed decoder to write fresh aircraft JSON in
one of these directories, or select its HTTP address in Settings. VectorWarp
does not install or start a decoder as part of discovery.

Automatic discovery also checks the existing loopback HTTP addresses. Stale,
malformed, oversized and symlinked files are rejected. Multiple healthy files
for one decoder are reported as ambiguous, rather than silently choosing one.
Linux keeps its existing `/run` paths.
