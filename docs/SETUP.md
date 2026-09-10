# Advanced setup

For a supported packaged install, start with [First install](INSTALL.md).
This guide is for source builds, development, and receiver SDKs not included in
the release package. VectorWarp runs natively; it has no Docker deployment.

## Build a Kraken artifact

Build as a normal user. Ubuntu 24.04 needs:

```bash
sudo apt update
sudo apt install build-essential cmake git curl tar zip unzip pkg-config \
  libfftw3-dev libarmadillo-dev
```

Fedora needs:

```bash
sudo dnf install gcc-c++ cmake make git curl tar zip unzip pkgconf-pkg-config \
  fftw-devel armadillo-devel
```

Install Node.js 22 or newer with npm from a supported source such as the
[official Node.js downloads](https://nodejs.org/en/download). Do not assume a
distribution's older default `nodejs` package satisfies the build script.

Then clone, preflight and build. `--gpu auto` enables the optional Vulkan path
only when its dependencies are available; `--gpu off` forces CPU processing.

```bash
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
cd blah2-VectorWarp
script/build-native.sh --preflight --backend kraken --gpu auto
script/build-native.sh --backend kraken --gpu auto
sudo script/install-native.sh --preflight
sudo script/install-native.sh
```

The build writes `build/native/artifact`; installation creates a versioned
release under `/opt/vectorwarp`, preserves an existing `/etc/vectorwarp/config.yml`,
and does not start services. Start with [First install](INSTALL.md) after it is
installed.

Other Linux distributions can use this source route when their compiler,
dependencies and receiver SDKs are compatible, but they are not packaged or
release-tested targets.

For a Vulkan build, install `libvulkan-dev glslang-dev glslang-tools` on Ubuntu
or `vulkan-loader-devel glslang-devel vulkan-tools` on Fedora. GPU drivers remain
the operating system/vendor's responsibility. See [GPU acceleration](GPU_ACCELERATION.md)
for behavior and fallback.

## All receiver SDK build

The source-only all-backend artifact includes RSPduo, USRP, dual HackRF and
Kraken after their SDKs are installed:

```bash
script/build-native.sh --preflight --backend all --gpu auto
script/build-native.sh --backend all --gpu auto
```

Ubuntu package names are `libhackrf-dev libuhd-dev uhd-host
libusb-1.0-0-dev`; Fedora names are `hackrf-devel uhd-devel libusb1-devel`.
Use UHD 4.8 or newer. Install SDRplay API 3.15.2 from its vendor archive after
reviewing its licence and instructions. Do not use an unreviewed network script
with elevated privileges.

Receiver access is host-specific. Grant the `vectorwarp` account only the needed
device groups (often `plugdev`, `dialout`, `render` or `video`) and verify the
radio/clock/cabling separately. The browser checks saved settings; it does not
prove receiver discovery or driver health.

## KrakenSDR Suite V2

VectorWarp consumes the calibrated TCP stream from
[KrakenSDR Suite V2](https://github.com/krakenrf/krakensdr_suite). Run
`heimdall_v2`, configure frequency and gain in Suite V2, and make these settings
agree with VectorWarp:

```yaml
capture:
  fs: 2400000
  fc: 204640000
  device:
    type: Kraken
    heimdall:
      host: 127.0.0.1
      port: 8091
    channel_count: 5
    reference_channel: 0
    surveillance_channels: [0, 1, 2, 3, 4]
```

Two through eight channels are accepted. In dedicated mode the reference is
excluded from surveillance; array-reference mode can synthesize a common
reference and fuse surveillance maps. Configure receiver and transmitter site
coordinates for display/evaluation geometry.

## Recording, replay and browser access

Portable `.blah2iq` recordings carry channel-major complex-float32 samples,
sample rate, centre frequency and channel count. Replay validates those values
and runs without opening a receiver. Legacy RSPduo int16, Kraken MCHQ, USRP
float32 and HackRF signed-int8 recordings require the selected replay format;
legacy block length is required where prompted.

The browser/API is served from one origin on port 3000 by default. Keep it on a
trusted LAN/VPN or place it behind an authenticated gateway. The settings UI has
no password. Raw ADS-B input is projected inside VectorWarp for display and
evaluation only; it is never detection or tracking input.

## Evidence and limits

See [upstream comparison](UPSTREAM_COMPARISON.md) for implemented, tested,
planned and unpublished work relative to upstream. The
[recorded-IQ benchmark](RECORDED_IQ_BENCHMARK.md) is frozen evidence from before
later detector/math fixes, so it is not a measurement of those fixes.
