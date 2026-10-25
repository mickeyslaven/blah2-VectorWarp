# VectorWarp documentation

Use this page to choose current instructions. Pages outside the first four
sections are design, test, research, or historical evidence; they are not an
installation contract or an operational capability claim.

## Install

- [Install VectorWarp](INSTALL.md): supported Linux packages, Homebrew, manual artifacts, and source builds.
- [macOS Homebrew](MACOS_HOMEBREW.md), [macOS runtime and development](MACOS.md), and [standalone macOS installer](MACOS_STANDALONE.md).
- [DragonOS package selection](DRAGONOS.md).
- [Raspberry Pi 4 preview](PI4_GUIDE.md): an advanced compatibility preview, not a supported primary deployment.

## Configure and operate

- [Receiver setup](SETUP.md): Kraken, RSPduo, USRP, HackRF, ADS-B, recording, replay, browser access, and service controls.
- [SDRplay setup](SDRPLAY_SETUP.md) and [receiver compatibility watcher](RECEIVER_COMPATIBILITY.md).
- [GPU acceleration](GPU_ACCELERATION.md) and [GPU diagnostics](GPU_DIAGNOSTICS.md).
- [macOS local Kraken](MACOS_KRAKEN.md) and [macOS GPU processing](MACOS_GPU.md).

## Develop and release

- [Package acceptance checks](PACKAGE_TESTING.md) and [packaging notes](../packaging/README.md).
- [Maintainer release guide](MAINTAINER_RELEASE.md), [Homebrew publishing](HOMEBREW_PUBLISHING.md), and [macOS release agent](MACOS_RELEASE_AGENT.md).
- [Receiver setting trace matrix](SDR_SETTING_TRACE_MATRIX.md) and [upstream comparison](UPSTREAM_COMPARISON.md).

## Research and experimental paths

[Advanced research](ADVANCED_RESEARCH.md) is the entry point for experimental
bearing, shadow, scorecard, and evaluation work. The source material and
evidence ledgers live in [research/](research/); none of these paths is enabled
or promised by the supported install and operation guides.

## Historical evidence and audits

Date-stamped benchmark, validation, acceptance, review, and remediation files
preserve the evidence available at the time they were written. They do not
override current instructions or configuration defaults. Start with
[the historical-record index](archive/README.md); benchmark receipts also live
under [benchmarks/](benchmarks/) and validation receipts under [validation/](validation/).
