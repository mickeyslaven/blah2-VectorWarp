<p><img src="html/favicon/vectorwarp-vw.svg" width="64" height="64" alt="VectorWarp"></p>

# VectorWarp

> **THIS IS A DEVELOPMENT BUILD. EXPECT BUGS AND REPORT VIA GITHUB ISSUES PLEASE AND THANK YOU!**

VectorWarp is a native passive-radar application for Linux and macOS. It offers
CPU or optional GPU delay--Doppler processing, supported receiver setup, browser
controls, recording, replay, and ADS-B display integration.

[Install](docs/INSTALL.md) · [macOS / Homebrew](docs/MACOS_HOMEBREW.md) · [Set up a receiver](docs/SETUP.md) · [Recording and replay](docs/SETUP.md#recording-replay)

## Install

1. **Linux:** install the matching signed DEB or RPM from the
   [package page](https://mickeyslaven.github.io/blah2-VectorWarp/#install).
2. **macOS:** use [Homebrew](docs/MACOS_HOMEBREW.md), or a signed PKG when the
   package page lists one.
3. **Development or unsupported systems:** follow the
   [source-install instructions](docs/INSTALL.md#build-from-source).

Open Settings, configure a receiver or replay file, and choose **Save & Restart**.
RSPduo users must install the SDRplay vendor API themselves, then choose
**Build SDRplay support**; VectorWarp does not distribute vendor software.

## Supported capabilities

- Optional GPU acceleration with CPU fallback.
- Coherent multi-channel input for compatible Kraken receivers.
- Receiver-setting checks and browser-based configuration.
- Recording and replay of IQ data.
- Local or remote ADS-B display feeds.
- Native Linux packages and macOS Homebrew installation.

Supported packaged Linux targets are Ubuntu 22.04/24.04/26.04, Debian 12/13,
Fedora 44, and matching DragonOS bases. Apple Silicon macOS is tested on M2;
Intel macOS remains experimental source-level CI only. See the full
[installation guide](docs/INSTALL.md) for the exact OS and SDR support matrix.

## Advanced and research material

Scorecards, array bearing/manifold work, and shadow evaluation paths are not
production capability promises. Their contracts and limitations are collected in
[advanced research documentation](docs/ADVANCED_RESEARCH.md).

## Advanced preview: Raspberry Pi 4B

Raspberry Pi 4B is an advanced compatibility preview, not a supported primary
platform. Its constrained workloads can miss real-time deadlines; prefer a
supported x86-64 PC or Apple Silicon Mac. See the [Pi 4 preview guide](docs/PI4_GUIDE.md)
and its [validation limits](docs/PI4_IMAGE_VALIDATION_20260919.md).

## Credits and license

Built on [blah2](https://github.com/30hours/blah2). [MIT license](LICENSE).
