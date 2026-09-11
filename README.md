# VectorWarp

VectorWarp is a native Linux, CPU-multithreaded and optionally
GPU-accelerated real-time radar fork of
[blah2](https://github.com/30hours/blah2) by 30hours. Performance depends on
the host, configuration and workload.

## Highlights

- KrakenSDR Suite V2 network input with 2–8-channel processing.
- Browser configuration, recording and replay controls. Apply settings with
  **Save & Restart**; replay does not open a receiver.
- Dedicated-reference processing for RSPduo, USRP and dual HackRF; coherent
  Kraken array-reference synthesis and map fusion.
- Optional Vulkan/VkFFT acceleration for the delay–Doppler stage, with
  automatic CPU fallback.
- Built-in ADS-B display/evaluation projection, with local decoder discovery
  or a remote tar1090 endpoint. ADS-B never informs
  detection or tracking.

## Processing performance

**Live five-channel processing and wider Doppler that keeps pace.** On a Ryzen
AI Max+ 395 / Radeon 8060S, with eight physical CPU cores available, the actual
processor measured the following with live five-input Kraken RF at 2.4 MS/s:

| Processed workload | Frame deadline | CPU ms/CPI | GPU ms/CPI |
| --- | ---: | ---: | ---: |
| Reference/surveillance pair, ±1600 Hz | 100 ms | 52.0 | 42.6 |
| Reference/surveillance pair, ±2400 Hz | 200 ms | 116.9 | 100.0 |
| Five-channel synthesized-reference array, ±800 Hz | 200 ms | 147.5 | 158.0 |
| Reference/surveillance pair, ±4000 Hz | 200 ms | 167.3 | 138.7 |
| Five-channel synthesized-reference array, ±1600 Hz | 400 ms | 368.2 | 427.7 |

Each mode ran 30 CPIs; the table averages the last 27. Every listed mode met all
27 processing deadlines **except** the 400 ms five-channel GPU case, which
missed all 27. CPU was better for these five-channel workloads. The ±4000 Hz
case uses 256 delay bins and exceeds regular blah2's safe Doppler buffer size.
These short live runs are not a long-duration, loss-free acquisition guarantee.

**Matched live-speed replay also shows useful GPU headroom.** On an RTX 4050
Laptop, the same recorded IQ and 200 ms / ±800 Hz pair workload averaged
**239 ms/CPI for regular blah2, 220 ms for VectorWarp CPU, and 162 ms for
VectorWarp GPU** in two alternating sample-clock-paced repeats. GPU used about
**32% less processing time** than upstream and met all 34 warm processing
deadlines; upstream missed 33. A heavier 250 ms / ±2000 Hz case was faster on
GPU but still missed every deadline. This comparison uses unchanged upstream
DSP, not its native acquisition service; documented detection/tracking fixes
mean full outputs are not identical.

See [live timings, matched replay, per-CPI data and limitations](docs/LIVE_CAPACITY_20260910.md).
Earlier tests also measured [47% less five-channel CPU time from channel workers](docs/ARRAY_CAPACITY_20260910.md)
and [three-repeat GPU results](docs/PER_CPI_BENCHMARK_20260910.md). CPU-only
comparisons do not establish a broad speedup, and Pi tests did not achieve
real time. Unsupported signed-delay windows are now rejected before processing.

### Verification

Automated coverage includes 2–8-channel parsing/unit/replay cases, API/browser
tests and offline GPU comparisons. Physical checks cover five-channel Kraken
input and the devices listed in [GPU hardware tests](docs/GPU_HARDWARE_TESTS.md).
A Fedora 44 x86-64 native installation and live smoke test have also passed;
this is not installation proof for every target OS or physical eight-channel
receiver validation. A Raspberry Pi 4 running Fedora 44 installed the ARM64 RPM
and passed 16 offline replay/startup cases; the tested processing profiles were
not real-time. Raspberry Pi OS and DragonOS device tests remain pending.
Release packages and the signed repository are not yet published. See the
[upstream comparison](docs/UPSTREAM_COMPARISON.md) for the detailed evidence.

## Install on Linux

VectorWarp has no Docker deployment. The package install includes its private
Node 24 runtime and serves both the browser UI and API on port 3000 by default;
no separate web server is required.

### Signed package repository — pending first release

The automatic signed repository and release packages are **not published yet**.
They become available only after the first verified GitHub Actions release and
GitHub Pages publication. Until that happens, use the advanced source build
below. Do not treat a planned URL or unsigned third-party package as official.

Once the first signed release is published, the systems listed under
[OS support](#os-support) will receive normal APT/DNF updates from our repository.

The verified repository bootstrap will be published at
`https://mickeyslaven.github.io/blah2-VectorWarp/install.sh`.
Download it, inspect it, then run it as root—never pipe it directly to a shell:

```bash
curl -fLO https://mickeyslaven.github.io/blah2-VectorWarp/install.sh
less install.sh
sudo bash install.sh --start-web
```

The installer will validate the published `keys/vectorwarp.asc` key and the
configured signing fingerprint before adding the APT or DNF repository. The
[public signing key](packaging/keys/vectorwarp.asc) and its trust fingerprint
are now configured; publication is still pending. `--start-web`
is optional and starts only the API/settings page; it never starts the processor
or radio.

Local release files, once published, can instead be installed with the normal
package manager:

```bash
sudo apt install ./vectorwarp_<version>-1_ubuntu24.04_amd64.deb
# or
sudo dnf install ./vectorwarp-<version>-1.fc44.x86_64.rpm
```

After installation, follow the short [first-run guide](docs/INSTALL.md): open
the browser, select the receiver, check the saved settings, then Save & Restart.
Receiver drivers, radio permissions and physical cabling remain host-specific.

## Advanced: build from source

Source builds are for development or for the all-receiver build. They are
not required for the release package's Kraken/Heimdall network receiver path.

```bash
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
cd blah2-VectorWarp
script/build-native.sh --preflight --backend kraken --gpu auto
script/build-native.sh --backend kraken --gpu auto
sudo script/install-native.sh --preflight
sudo script/install-native.sh
```

The build produces `build/native/artifact`; the installer creates a versioned
native install under `/opt/vectorwarp` and does not start services. For all four
receiver SDKs, GPU choices, dependencies and staging installs, see
[advanced setup](docs/SETUP.md).

## OS support

All ten Ubuntu, Debian and Fedora OS/architecture builds passed hosted build
and installation checks. Physical-device testing remains narrower, as noted
below. Release packages and the signed repository are **not published yet**.

| Operating system | Versions | Architectures | Package |
| --- | --- | --- | --- |
| Ubuntu | 22.04, 24.04, 26.04 | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | DEB for the matching Ubuntu version |
| Debian | 13 (Trixie) | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | Debian 13 DEB |
| Fedora | 44 | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | Fedora 44 RPM |
| DragonOS | Ubuntu 22.04, 24.04 or 26.04 base | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | Matching Ubuntu DEB, selected from OS metadata |
| Raspberry Pi OS | Trixie, 64-bit | ARM64 (arm64 / aarch64) | Debian 13 ARM64 DEB |

`amd64` and `x86_64` mean the same 64-bit Intel/AMD architecture—not AMD-only
support. Likewise, `arm64` and `aarch64` mean 64-bit ARM. Package filenames and
commands retain the names required by each distribution; choose the package
for your installed OS as well as your architecture.

Fedora 44 package/replay validation on Raspberry Pi 4 is
[recorded here](docs/PI4_VALIDATION_20260910.md). DragonOS ISO installation and
Raspberry Pi OS remain unverified.
See [DragonOS notes](docs/DRAGONOS.md), [Pi setup](docs/INSTALL.md#raspberry-pi),
and [current validation evidence](docs/UPSTREAM_COMPARISON.md).

Other Linux distributions may build from source but are not package-tested
targets. There are no 32-bit packages. macOS and Windows can use the browser UI;
they are not supported processor hosts.

## Receiver support

The current package build contains only the Kraken/Heimdall live backend.
Source builds can select the following backends with their external SDKs and
host permissions. This lists implemented support, not physical verification of
every receiver:

- [KrakenSDR](https://www.krakenrf.com/) Suite V2 / Heimdall network stream
- [SDRplay RSPduo](https://www.sdrplay.com/rspduo/)
- [USRP](https://www.ettus.com/products/) (upstream B210 coverage)
- Two synchronized [HackRF](https://greatscottgadgets.com/hackrf/) devices

RTL-SDR is retained only as an upstream compatibility reference and is not
selectable in this fork.

## Documentation

- [First install and browser setup](docs/INSTALL.md)
- [DragonOS package-selection notes](docs/DRAGONOS.md)
- [Advanced source build and receiver setup](docs/SETUP.md)
- [GPU acceleration](docs/GPU_ACCELERATION.md)
- [Current per-CPI timings and processing limits](docs/PER_CPI_BENCHMARK_20260910.md)
- [Five-channel CPU scaling and wider-Doppler limits](docs/ARRAY_CAPACITY_20260910.md)
- [Earlier recorded-IQ benchmark](docs/RECORDED_IQ_BENCHMARK.md)
- [Upstream comparison, math audit and validation evidence](docs/UPSTREAM_COMPARISON.md)
- [Maintainer release guide](docs/MAINTAINER_RELEASE.md)

The earlier recorded-IQ benchmark is frozen evidence from before later detector/math
fixes; it is useful baseline data, not a claim that those later fixes were
measured by that run.

For the exact change boundary, see the comparison against upstream blah2 and
the integration baseline in [UPSTREAM_COMPARISON.md](docs/UPSTREAM_COMPARISON.md).

## Security

The settings UI has no login. Keep it on a trusted LAN/VPN or behind an
authenticated gateway. Native services use unprivileged accounts; Save & Restart
is limited to the VectorWarp units and is not general shell access.

## Community and credit

VectorWarp preserves required blah2 wire/configuration compatibility. Upstream
authorship and the orange 30hours credit remain with
[30hours/blah2](https://github.com/30hours/blah2). For upstream discussion, see
the [blah2 Discord](https://discord.gg/ewNQbeK5Zn).

Contributions are welcome.

## Future work

Reliable Kraken bearing could eventually help locate returns on a map. It is
not reliable enough to influence this tracker. For multi-receiver location work,
see the upstream [3lips](https://github.com/30hours/3lips) project.

## License

[MIT](LICENSE)
