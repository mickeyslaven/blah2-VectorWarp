# Advanced setup

Use this guide for source builds, development, or live RSPduo, USRP, and
dual-HackRF. For a published Kraken package, start with [First install](INSTALL.md).
VectorWarp runs natively; it has no Docker deployment.

## Build a Kraken artifact

Install Node.js 22+ with npm, plus these build dependencies.

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git curl tar zip unzip pkg-config \
  libfftw3-dev libarmadillo-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake make ninja-build git curl tar zip unzip pkgconf-pkg-config \
  fftw-devel armadillo-devel
```

Build and install as follows. `--gpu auto` uses Vulkan only when available;
`--gpu off` forces CPU processing.

```bash
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
cd blah2-VectorWarp
script/build-native.sh --preflight --backend kraken --gpu auto
script/build-native.sh --backend kraken --gpu auto
sudo script/install-native.sh --preflight
sudo script/install-native.sh
```

The build creates `build/native/artifact`. Installation preserves an existing
`/etc/vectorwarp/config.yml` and does not start services. For Vulkan, install
`libvulkan-dev glslang-dev glslang-tools` on Ubuntu/Debian or
`vulkan-loader-devel glslang-devel vulkan-tools` on Fedora. Drivers remain the
OS/vendor responsibility; see [GPU acceleration](GPU_ACCELERATION.md).

Other distributions may work through this route when their compiler,
dependencies, and SDKs are compatible, but are not packaged release targets.

## Other receiver builds

Build one optional live backend with `--backend rspduo`, `--backend usrp`, or
`--backend hackrf`; each artifact also supports Kraken and replay. Build all
four live backends only after installing every SDK:

```bash
script/build-native.sh --preflight --backend all --gpu auto
script/build-native.sh --backend all --gpu auto
```

Install `libhackrf-dev libuhd-dev uhd-host libusb-1.0-0-dev` on Ubuntu, or
`hackrf-devel uhd-devel libusb1-devel` on Fedora. USRP needs UHD 4.8+.
RSPduo needs SDRplay API 3.15.2 from the vendor. Qualified add-only dependency
plans for USRP and HackRF require local enrollment, one-use approval and a
compiled backend; they do not install arbitrary drivers. Grant the `vectorwarp`
account only the required device groups and verify radio, clock, and cabling
separately. The browser validates saved settings; it does not prove hardware health.

## KrakenSDR Suite V2

VectorWarp consumes the calibrated TCP stream from
[KrakenSDR Suite V2](https://github.com/krakenrf/krakensdr_suite). Keep the
Kraken installation and USB driver under Suite V2's control. VectorWarp can
enroll, reuse and start an already-installed allowlisted local service, or use
a remote endpoint; it does not install Kraken drivers. Apply the browser
frequency and active channel prefix, then verify the receiver's readback at
processor startup:

```yaml
capture:
  fs: 2400000
  device:
    type: Kraken
    heimdall: {host: 127.0.0.1, port: 8091}
    channel_count: 5
    reference_channel: 0
    surveillance_channels: [0, 1, 2, 3, 4]
```

Kraken accepts two to eight coherent channels. Dedicated mode excludes the
reference from surveillance; array-reference mode synthesizes a common
reference. Set receiver/transmitter coordinates for geometry displays.

## Recording, replay and browser access

### ADS-B source

In **Settings → ADS-B planes**, discover a local readsb/dump1090 feed or enter
a remote tar1090 endpoint. Discovery neither installs nor starts a decoder,
controls an SDR, nor scans other hosts. The selected source is stored in
`truth.adsb.tar1090`. Live ADS-B is disabled during replay and preview.

### Recording replay

Portable `.blah2iq` recordings contain channel-major complex-float32 samples,
sample rate, frequency, and channel count. Replay validates those values and
does not open hardware. Legacy RSPduo, Kraken MCHQ, USRP float32, and HackRF
signed-int8 files require the matching replay format; legacy USRP blocks also
need their original block length.

The browser/API uses port 3000 by default and has no password. Keep it on a
trusted LAN/VPN or behind an authenticated gateway. ADS-B is display/evaluation
data only; it is never a detection or tracking input.

## Evidence and limits

See [upstream comparison](UPSTREAM_COMPARISON.md) for implemented, tested,
planned, and unpublished work. The [recorded-IQ benchmark](RECORDED_IQ_BENCHMARK.md)
is historical evidence, not a measurement of later detector/math changes.
