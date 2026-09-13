# Source build and receiver setup

This is the working installation route until the first package release is
published. See [Installation](INSTALL.md) for first startup and browser access.
VectorWarp runs natively; Docker is not required.

## Build from source

Install [Node.js](https://nodejs.org/en/download) 22 or newer with npm. The native services require the Node
executable at `/usr/bin/node`; a shell-only installation through a version
manager is not sufficient. Check both before building:

```bash
/usr/bin/node --version
npm --version
```

Install these build dependencies:

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git curl tar zip unzip pkg-config sudo python3 python3-apt \
  libfftw3-dev libarmadillo-dev libuhd-dev uhd-host libboost-dev libhackrf-dev libusb-1.0-0-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake make ninja-build git curl tar zip unzip pkgconf-pkg-config sudo python3 python3-libdnf5 python3-rpm \
  fftw-devel armadillo-devel uhd-devel boost-devel hackrf-devel libusb1-devel
```

For GPU support, also install `libvulkan-dev glslang-dev glslang-tools` on
Ubuntu/Debian or `vulkan-loader-devel glslang-devel vulkan-tools` on Fedora,
plus your GPU's Vulkan driver. `--gpu auto` builds the optional GPU path when
its dependencies are available; it does not install drivers. Use `--gpu off`
for a CPU-only build. See [GPU setup](GPU_ACCELERATION.md).

Choose a build target for the receivers you need:

| `--backend` | Live receivers included | Receiver software needed |
| --- | --- | --- |
| `kraken` | Kraken | Local or remote KrakenSDR Suite V2 |
| `usrp` | USRP and Kraken | UHD 4.1+ |
| `hackrf` | Dual HackRF and Kraken | libhackrf |
| `rspduo` | RSPduo and Kraken | Locally installed SDRplay API 3.15, including headers |
| `all` | All four | All of the above |

These are source-build options, not separate release-package products. Only
`rspduo` and `all` require the licensed SDRplay SDK at build time; install it
yourself using the [SDRplay instructions](SDRPLAY_SETUP.md). UHD and HackRF
development packages are included in the dependency commands above.

This example builds for Kraken. Change `VW_BACKEND` before running it if you
need a different target; omitting `--backend` defaults to `all`.

```bash
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
cd blah2-VectorWarp
VW_BACKEND=kraken
script/build-native.sh --backend "$VW_BACKEND" --preflight --gpu auto
script/build-native.sh --backend "$VW_BACKEND" --gpu auto
sudo script/install-native.sh --preflight
sudo script/install-native.sh
sudo systemctl start vectorwarp-api.service
```

The build creates `build/native/artifact`. Installation preserves an existing
`/etc/vectorwarp/config.yml` and does not start VectorWarp services. A first
RSPduo-enabled installation may start an already-installed standard SDRplay API
service; see [SDRplay setup](SDRPLAY_SETUP.md). The final command starts only the
API. Open `http://localhost:3000/` on that machine, or
`http://<server-IP>:3000/` from another device, then configure the receiver in
**Settings** before choosing **Save & Restart**. Neither service is enabled
at boot by these commands.

Other distributions may work through this route when their compiler,
dependencies, and SDKs are compatible, but are not packaged release targets.

Give the `vectorwarp` service account only the device access it needs.
Radio clocks, cabling and coherent reception still need a hardware check.

## KrakenSDR Suite V2

VectorWarp consumes the calibrated TCP stream from
[KrakenSDR Suite V2](https://github.com/krakenrf/krakensdr_suite). Keep the
Kraken installation and USB driver under Suite V2's control. VectorWarp can
enroll, reuse and start an already-installed allowlisted local service, or use
a remote endpoint; it does not install Kraken drivers. Apply the browser
frequency, gain and active channel prefix, then verify the receiver's readback at
processor startup. This fragment shows five-channel array-reference settings;
edit the installed configuration rather than replacing it with this fragment:

```yaml
capture:
  fs: 2400000
  device:
    type: Kraken
    heimdall: {host: 127.0.0.1, port: 8091, control_port: 8092}
    channel_count: 5
    reference_channel: 0
    surveillance_channels: [0, 1, 2, 3, 4]
process:
  reference_synthesis:
    mode: array_eigenbeam
    channels: [0, 1, 2, 3, 4]
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
| SDRplay RSPduo | `capture.fc`, `capture.fs`, and `capture.device.serial`, `agcSetPoint`, `bandwidthNumber`, `gainReduction`, `lnaState`, `dabNotch`, `rfNotch` are saved and applied through SDRplay API v3 at processor startup. | SDK return failures reach processor telemetry; there is no independent post-init tuner readback. Save & Restart starts a standard installed SDRplay API service or reuses an active one. Custom stopped services need local administrator review. | Not verified by software discovery alone. The vendor API must be installed under its license; VectorWarp does not download, license or redistribute it. |
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
planned, and unpublished work, and the [fixed-range benchmark](GPU_BENCHMARK_20260911.md)
for current processing times and test limits.
