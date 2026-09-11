<p><img src="html/favicon/vectorwarp-vw.svg" width="64" height="64" alt="VectorWarp"></p>

# VectorWarp

VectorWarp builds on [blah2](https://github.com/30hours/blah2) with faster GPU-assisted processing, a redesigned browser interface, editable settings, built-in ADS-B integration, and native Linux installation. Set up your radar, watch it run, and record or replay signals from the same interface.

[Install](docs/INSTALL.md) · [Set up a receiver](docs/SETUP.md) · [Comparison details](docs/UPSTREAM_COMPARISON.md) · [GPU acceleration](docs/GPU_ACCELERATION.md)

## What you get beyond blah2

- **GPU acceleration:** use a compatible GPU for delay–Doppler processing, with automatic selection and CPU fallback.
- **Wider Doppler coverage:** process valid delay/Doppler combinations that exceed the original processor's buffer limits. See the live example below.
- **Automatic CPU threading:** size channel workers and FFT threads to the CPU capacity available, with manual controls when you need them.
- **Settings in your browser:** edit receiver, processing, display, recording, and ADS-B settings in organized sections.
- **Checked inputs:** dropdowns, range checks, and short explanations help catch invalid settings before they reach the processor.
- **Save & Restart:** apply settings and follow restart progress, with an error message if startup fails.
- **Recoverable configuration:** backups and file-change checks help protect your settings, including when you also edit the config file directly.
- **Kraken configuration checks:** compare the receiver's reported frequency, sample rate, and channel count with VectorWarp's settings.
- **A redesigned interface:** clearer navigation, readable controls, live status, and fullscreen radar views.
- **Delay ellipses on a map:** see the possible locations associated with a measured delay, without presenting it as an exact aircraft position.
- **Built-in ADS-B conversion:** the adsb2dd integration runs inside VectorWarp; no separate converter process to manage.
- **Local or remote aircraft feeds:** discover a local decoder or connect to a tar1090 server on another machine.
- **Clear recording controls:** start recording from the browser or keyboard, with a visible recording indicator and error reporting.
- **Portable recording and replay:** capture multi-channel IQ, replay it at the original sample rate, and use recordings without connecting live hardware.
- **Processing fixes:** corrected detection SNR, track association, spectrum scaling, and invalid-data handling, with regression tests.
- **Native Linux operation:** run without Docker, with build and package targets for Ubuntu, Debian, Fedora, DragonOS, and 64-bit Raspberry Pi systems.

[Detailed changes from upstream](docs/UPSTREAM_COMPARISON.md) · [Recording and replay](docs/SETUP.md#recording-replay)

## Faster processing in practice

Same recorded signal, same settings, and the same CPU allocation on each machine.
These are processing times per 200 ms radar frame (CPI); lower is better.

| Hardware | Regular blah2 CPU | VectorWarp CPU | VectorWarp GPU | GPU time saved vs blah2 |
| --- | ---: | ---: | ---: | ---: |
| RTX 4050 Laptop | **239 ms** | **221 ms** | **162 ms** | **32%** |
| Ryzen AI Max+ 395 / Radeon 8060S | **76 ms** | **74 ms** | **62 ms** | **19%** |

On the RTX 4050 laptop, VectorWarp GPU met **all 34** steady-frame deadlines;
regular blah2 missed **33**. That is the difference between keeping up with this
200 ms signal stream and falling behind.

**A wider live example:** VectorWarp processed **±4000 Hz Doppler in 139 ms per
200 ms frame** on the Ryzen system, at 2.4 MS/s with 256 delay bins. Regular
blah2 cannot safely process that configuration because of its buffer sizing.

The table times signal processing during replay at the original sample rate:
two repeats at ±800 Hz, with four CPU cores on the laptop and eight on the Ryzen
system. The live example averages 27 steady frames. GPU results here accelerate
the delay–Doppler stage.
[Full settings, results, and test method →](docs/LIVE_CAPACITY_20260910.md)

## Install on Linux

Start with [the installation guide](docs/INSTALL.md), then follow [receiver setup](docs/SETUP.md).

- **KrakenSDR Suite V2:** 2–8-channel network input, synthesized or dedicated reference, and combined surveillance maps. Included in package builds.
- **SDRplay RSPduo, USRP, and dual HackRF:** source builds with the receiver's required SDK.

The Kraken multi-channel foundation is also offered to blah2 in
[our upstream PR #45](https://github.com/30hours/blah2/pull/45).
VectorWarp adds the interface, integration, and processing improvements above;
the PR remains separate.

## OS support

The first signed APT/DNF release is being prepared. Source installation is
available now; package targets are listed below.

| Operating system | Versions | Architectures | Package |
| --- | --- | --- | --- |
| Ubuntu | 22.04, 24.04, 26.04 | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | DEB for the matching Ubuntu version |
| Debian | 13 (Trixie) | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | Debian 13 DEB |
| Fedora | 44 | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | Fedora 44 RPM |
| DragonOS | Ubuntu 22.04, 24.04, or 26.04 base | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | Matching Ubuntu DEB selected from OS metadata |
| Raspberry Pi OS | Trixie, 64-bit | ARM64 (arm64 / aarch64) | Debian 13 ARM64 DEB |

## Credits and license

Thanks to [30hours](https://github.com/30hours/blah2) for the original radar
engine. Comparisons use [blah2 at c821bee](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de).
ADS-B is used for display and evaluation, not radar detection or tracking input.
[MIT license](LICENSE)
