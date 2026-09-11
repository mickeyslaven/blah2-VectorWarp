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

On an RTX 4050 Laptop, VectorWarp GPU used **28% less processing time per CPI**
than upstream blah2 in the matched 200 ms / ±800 Hz test: **163 vs 228 ms/CPI**.
At that setting the GPU finished all 51 measured warm frames within the 200 ms
budget; upstream exceeded it in 50. Lower milliseconds means more processing
headroom—not a guarantee for every radio, computer or configuration.

| CPI / Doppler span | Upstream CPU | VectorWarp CPU | VectorWarp GPU |
| --- | ---: | ---: | ---: |
| 200 ms / ±800 Hz | 228 ms/CPI | 224 ms/CPI | 163 ms/CPI |
| 200 ms / ±1200 Hz | 247 ms/CPI | 244 ms/CPI | 186 ms/CPI |
| 50 ms / ±1600 Hz | 79 ms/CPI | 86 ms/CPI | 59 ms/CPI |

These are three-repeat instrumented DSP measurements on the **same laptop**,
using identical real recorded IQ, settings, one reference/surveillance pair,
four CPU cores and four FFT threads. The ±1200 Hz GPU test still missed 3/51
deadlines; the 50 ms test did not keep up. CPU-only and Pi tests do not establish
a reliable speedup. Upstream output differences from documented detection and
tracking fixes are disclosed in the [full results](docs/PER_CPI_BENCHMARK_20260910.md).

Separately, the **actual VectorWarp processor's CPI timing stream** measured
239 ms on CPU versus 180 ms on GPU in sample-rate-paced replay of the same
200 ms / ±800 Hz setup. GPU missed 1/17 warm deadlines, CPU 17/17. This includes
the application's processing/output path, but **is not a live-radio test** or
an upstream full-application comparison. See the report for startup costs,
deadline counts, provenance and limitations.

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
