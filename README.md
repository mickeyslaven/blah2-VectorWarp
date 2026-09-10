# VectorWarp

VectorWarp is a native Linux, CPU-multithreaded and optionally
GPU-accelerated real-time radar. It is a faster-processing fork of
[blah2](https://github.com/30hours/blah2) by 30hours. Performance depends on
the host and configuration.

## Highlights

- KrakenSDR Suite V2 network input with 2–8-channel processing.
- Browser configurable, allowing for on the fly changes, and even replaying old recordings right in browser. 
- Dedicated-reference processing for RSPduo, USRP and dual HackRF; coherent
  Kraken array-reference synthesis and map fusion.
- Optional Vulkan/VkFFT acceleration with automatic CPU fallback.
- Built-in raw ADS-B display/evaluation projection. ADS-B never informs
  detection or tracking.

## Install on Linux

VectorWarp has no Docker deployment. The package install includes its private
Node 24 runtime and serves both the browser UI and API on port 3000 by default;
no separate web server is required.

### Signed package repository — pending first release

The automatic signed repository and release packages are **not published yet**.
They become available only after the first verified GitHub Actions release and
GitHub Pages publication. Until that happens, use the advanced source build
below. Do not treat a planned URL or unsigned third-party package as official.

Once the first signed release is published, supported systems will install from
the single maintained repository and receive normal APT/DNF updates:

- Ubuntu 22.04, 24.04 or 26.04, amd64 or arm64
- DragonOS editions whose `/etc/os-release` identifies one of those Ubuntu
  bases, amd64 or arm64
- Fedora 44, x86_64 or aarch64

DragonOS is selected through its Ubuntu base metadata, not its independent ISO
release label. The installer has metadata-fixture coverage, but no DragonOS ISO
has been boot-tested here; see [DragonOS notes](docs/DRAGONOS.md).

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
fingerprint is not configured until the release signing key exists. `--start-web`
is optional and starts only the API/settings page; it never starts the processor
or radio.

Local release files, once published, can instead be installed with the normal
package manager:

```bash
sudo apt install ./vectorwarp_<version>-1_ubuntu24.04_amd64.deb
# or
sudo dnf install ./vectorwarp-<version>-1.fc44.x86_64.rpm
```

Other Linux distributions are not packaged or release-tested, but may use the
source-build route when their dependencies are compatible. macOS and Windows
are browser clients, not VectorWarp processor hosts.

After installation, follow the short [first-run guide](docs/INSTALL.md): open
the browser, select the receiver, check the saved settings, then Save & Restart.
Receiver drivers, radio permissions and physical cabling remain host-specific.

## Advanced: build from source

Source builds are for development or for the all-receiver SDK build. They are
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

## Receiver support

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
- [Recorded-IQ benchmark](docs/RECORDED_IQ_BENCHMARK.md)
- [Upstream comparison, math audit and validation evidence](docs/UPSTREAM_COMPARISON.md)
- [Maintainer release guide](docs/MAINTAINER_RELEASE.md)

The recorded-IQ benchmark is frozen evidence from before later detector/math
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
