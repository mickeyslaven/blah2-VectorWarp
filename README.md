<p><img src="html/favicon/vectorwarp-vw.svg" width="64" height="64" alt="VectorWarp"></p>

# VectorWarp

Fast passive radar for Linux and macOS. VectorWarp provides CPU or optional GPU
delay--Doppler processing, checked receiver setup, browser controls, recording,
replay, and ADS-B display integration.

[Install](docs/INSTALL.md) · [Set up a receiver](docs/SETUP.md) · [Documentation map](docs/README.md) · [Recording and replay](docs/SETUP.md#recording-replay)

## Install and start

1. **Linux:** use the signed APT or DNF repository from the
   [package page](https://mickeyslaven.github.io/blah2-VectorWarp/#install).
2. **macOS:** use [Homebrew](docs/MACOS_HOMEBREW.md), or a signed PKG when the
   package page lists one.
3. **Development or unsupported systems:** follow the
   [source-install instructions](docs/INSTALL.md#build-from-source).

Open Settings, configure a receiver or replay file, then choose **Save & Restart**.
RSPduo users must install the SDRplay vendor API themselves, then choose
**Build SDRplay support**; VectorWarp does not distribute vendor software.

## Proven features

- Optional GPU acceleration with CPU fallback.
- Coherent multi-channel input for compatible Kraken receivers.
- Receiver-setting checks and browser-based configuration.
- Recording and replay of IQ data.
- Local or remote ADS-B display feeds.
- Native Linux packages and macOS Homebrew installation.

Packages support Ubuntu 22.04/24.04/26.04, Debian 12/13, Fedora 44, and
matching DragonOS bases. Apple Silicon macOS is tested on M2; Intel macOS is
experimental source-level CI only. See the [installation guide](docs/INSTALL.md)
for the OS and SDR support matrix.

## Advanced and research material

Advanced scorecards, array bearing/manifold work, and shadow evaluation are
research paths, separate from the supported install and operation guides. See
[advanced research documentation](docs/ADVANCED_RESEARCH.md).

## Advanced preview: Raspberry Pi 4B

Raspberry Pi 4B is an advanced compatibility preview, not a supported primary
platform. Its constrained workloads can miss real-time deadlines; prefer a
supported x86-64 PC or Apple Silicon Mac. See the [Pi 4 preview guide](docs/PI4_GUIDE.md)
and its [validation limits](docs/PI4_IMAGE_VALIDATION_20260919.md).

## Credits and license

Built on [blah2](https://github.com/30hours/blah2). [MIT license](LICENSE).
