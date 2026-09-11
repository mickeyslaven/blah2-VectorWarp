# Advanced setup

Most users should start with [First install](INSTALL.md). Each package includes
all supported receiver adapters; there is no SDR-specific package to choose.
This guide covers source builds and receiver connections.
VectorWarp runs natively; it has no Docker deployment.

## Build from source

Install Node.js 22+ with npm, plus these build dependencies.

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git curl tar zip unzip pkg-config \
  libfftw3-dev libarmadillo-dev libuhd-dev uhd-host libboost-dev libhackrf-dev libusb-1.0-0-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake make ninja-build git curl tar zip unzip pkgconf-pkg-config \
  fftw-devel armadillo-devel uhd-devel boost-devel hackrf-devel libusb1-devel
```

Boost headers are required by UHD's public API, and HackRF's `pkg-config`
metadata can reference libusb headers without pulling in their development
package. Both are listed explicitly; installing only the receiver runtime or
its nominal development package is not enough on every supported distribution.

Install SDRplay API 3.15 from the vendor to supply its licensed build headers
and library. Release builders can use the approved build-only SDK extraction
described in [packaging](../packaging/README.md).

Build and install as follows. `--gpu auto` uses Vulkan only when available;
`--gpu off` forces CPU processing.

```bash
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
cd blah2-VectorWarp
script/build-native.sh --preflight --gpu auto
script/build-native.sh --gpu auto
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

## Development-only reduced builds

The default source build includes all four adapters. Developers can omit
unneeded SDKs with `--backend kraken`, `rspduo`, `usrp` or `hackrf`; these are
not separate release products. USRP supports UHD 4.1 or newer.
Give the `vectorwarp` service account only the device access it needs.
Radio clocks, cabling and coherent reception still need a hardware check.

## KrakenSDR Suite V2

VectorWarp consumes the calibrated TCP stream from
[KrakenSDR Suite V2](https://github.com/krakenrf/krakensdr_suite). Keep the
Kraken installation and USB driver under Suite V2's control. VectorWarp can
enroll, reuse and start an already-installed allowlisted local service, or use
a remote endpoint; it does not install Kraken drivers. Apply the browser
frequency, gain and active channel prefix, then verify the receiver's readback at
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

## Receiver settings: application and verification

Choosing a receiver and saving settings does not prove a radio is present. The
Receiver software check separately reports whether the adapter was compiled,
whether its runtime module loads, and any read-only discovery evidence. It never
retunes a device or replaces the saved receiver/remote endpoint.

| Receiver | Settings sent to software | Verification boundary | Physical hardware status |
| --- | --- | --- | --- |
| KrakenSDR Suite V2 | `capture.fc`, `capture.device.channel_count`, and an explicit `capture.device.heimdall.gain` go through Suite TCP control. Gain defaults to `keep` (and an absent old setting also preserves the receiver); `-1` requests Suite automatic gain and 0–50 is manual dB. `capture.fs` is read from Suite status, not set at runtime. Endpoint, reference/surveillance selection and synthesis stay in VectorWarp. | Apply requires the command ACK and a subsequent fresh Suite status readback for each implemented frequency, element-count or explicit-gain control. A sample-rate mismatch blocks Apply. | Software/status only unless a separate receiver run is recorded. A Suite-reported gain is not an actual RF-gain proof. Generic RTL USB descriptors are never treated as Kraken identity. |
| SDRplay RSPduo | `capture.fc`, `capture.fs`, and `capture.device.serial`, `agcSetPoint`, `bandwidthNumber`, `gainReduction`, `lnaState`, `dabNotch`, `rfNotch` are saved and applied through SDRplay API v3 at processor startup. | SDK return failures reach processor telemetry; there is no independent post-init tuner readback. An installed running API is reused; a stopped API can only be started through an already enrolled, approved action. | Not verified by software discovery alone. The vendor API must be installed under its license; VectorWarp does not accept or redistribute it. |
| Ettus USRP / B210 | `capture.fc`, `capture.fs`, and `capture.device.address`, `subdev`, `antenna`, `gain` are UHD startup parameters after **Save & Restart**. | Before IQ streaming, UHD getters check both channels (rate ±0.5 Hz, tuning ±1 Hz, gain ±0.05 dB, exact antenna/subdevice). This is not an instant browser setter or an RF/clock-source proof. | Not verified by software discovery alone. UHD 4.1+ and a compiled adapter are separate requirements; clock/time source is not currently an exposed setting. |
| Dual HackRF | `capture.fc`, `capture.fs`, and `capture.device.serial`, `gain_lna`, `gain_vga`, `amp_enable` are applied to the two selected serials at processor startup. | Open/set/start return codes are checked; there is no post-set frequency, gain, clock or synchronization readback. | Not verified by software discovery alone; two configured matching serials are required. |

The USRP and HackRF software paths also pass 6 MS/s replay checks with clutter
filtering. These are correctness tests, not sustained hardware-throughput tests.
Upstream documents a B210 timeout/crash after 5–10 minutes. Defensive receive
error handling does not establish that its underlying hardware/driver issue is
fixed; a physical B210 endurance test is still needed.

The settings page can show this same field-by-field matrix for every profile.
For a remote Kraken endpoint it performs status/control only at that configured
endpoint and never manages a local Suite service.

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
