<p><img src="html/favicon/vectorwarp-vw.svg" width="64" height="64" alt="VectorWarp"></p>

# VectorWarp

### More channels. Wider Doppler. Faster radar.

Native Linux passive radar with multicore processing, optional GPU acceleration,
and a complete browser interface. Built on [blah2](https://github.com/30hours/blah2).

[Get started](docs/INSTALL.md) · [Performance results](docs/LIVE_CAPACITY_20260910.md) · [GPU setup](docs/GPU_ACCELERATION.md)

## Faster than regular blah2

Same recorded signal. Same settings. Same CPU allocation on each machine.

| Hardware | Regular blah2 CPU | VectorWarp GPU | Less processing time |
| --- | ---: | ---: | ---: |
| RTX 4050 Laptop | **239 ms** | **162 ms** | **32%** |
| Ryzen AI Max+ 395 / Radeon 8060S | **76 ms** | **62 ms** | **19%** |

At a **200 ms frame deadline**, the RTX 4050 result brings processing into real
time: VectorWarp GPU met all **34** steady-frame deadlines; regular blah2 missed **33**.

Measured DSP means over two repeats of 200 ms / ±800 Hz sample-clock-paced DSP replay;
four CPU cores on the laptop, eight on the Ryzen system.

## More radar per frame

- **Five-channel live processing in 148 ms:** synthesized reference and five
  surveillance maps within a 200 ms frame, beyond regular blah2's two-channel path.
- **Live ±4000 Hz Doppler in 139 ms on GPU:** within a 200 ms frame, at 2.4 MS/s
  with 256 delay bins. **Regular blah2 cannot safely process this configuration.**

Live examples: Ryzen AI Max+ 395 / Radeon 8060S, eight CPU cores, 2.4 MS/s;
means over 27 steady frames. The five-channel example uses CPU at ±800 Hz.

[See the configurations, full results and methodology →](docs/LIVE_CAPACITY_20260910.md)

## Everything in one interface

- **Live radar and maps:** delay–Doppler, delay ellipses, reference spectrum and fullscreen displays.
- **Browser settings:** organized controls, validation, clear errors and **Save & Restart**.
- **Built-in ADS-B:** discover a local decoder or connect to a remote tar1090 server.
- **Record and replay:** capture IQ and revisit recordings through the same interface.
- **Automatic processing choices:** host-aware CPU threading and GPU selection with CPU fallback.

## Install on Linux

VectorWarp runs natively, with its own web interface and runtime.

The first signed APT/DNF release is being prepared. Start with the
[source-build guide](docs/SETUP.md), then follow
[browser setup](docs/INSTALL.md#configure-and-receive).

## OS support

Package targets use the native architecture names shown below:

| Operating system | Versions | Architectures | Package |
| --- | --- | --- | --- |
| Ubuntu | 22.04, 24.04, 26.04 | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | DEB for the matching Ubuntu version |
| Debian | 13 (Trixie) | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | Debian 13 DEB |
| Fedora | 44 | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | Fedora 44 RPM |
| DragonOS | Ubuntu 22.04, 24.04, or 26.04 base | x86-64 (amd64 / x86_64), ARM64 (arm64 / aarch64) | Matching Ubuntu DEB selected from OS metadata |
| Raspberry Pi OS | Trixie, 64-bit | ARM64 (arm64 / aarch64) | Debian 13 ARM64 DEB |

[Platform and installation notes](docs/INSTALL.md)

## Receiver support

- **KrakenSDR Suite V2:** 2–8-channel network input, included in package builds.
- **SDRplay RSPduo, USRP and dual HackRF:** source builds with the receiver's SDK.

[Receiver setup](docs/SETUP.md#other-receiver-builds) · [Recording and replay](docs/SETUP.md#recording-replay)

## Built on blah2

Thanks to [30hours](https://github.com/30hours/blah2) for the original radar
engine. VectorWarp builds on that work with multi-channel processing, GPU
acceleration and a browser-first experience.

[Changes from upstream](docs/UPSTREAM_COMPARISON.md) · [MIT license](LICENSE)
