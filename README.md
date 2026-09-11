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

VectorWarp's optional Vulkan GPU path can make a practical radar workload keep
up where regular blah2 falls behind. CPU mode remains available, and Automatic
mode checks accuracy before retaining GPU work.

| Same recorded IQ, same CPU budget | Regular blah2 CPU | VectorWarp CPU | VectorWarp GPU |
| --- | ---: | ---: | ---: |
| RTX 4050 Laptop, 200 ms CPI, ±800 Hz | 240.1 ms | 239.5 ms | **127.8 ms** |
| Pavilion AMD GPU, 200 ms CPI, ±2400 Hz | 308.1 ms | 303.2 ms | **174.6 ms** |

The comparison used actual [30hours/blah2 at `c821bee`](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de)
and VectorWarp on each host, replaying the same recorded signal at its original
rate. On the RTX workload, regular blah2 missed 23 of 24 measured 200 ms
intervals while VectorWarp GPU missed 2. Periodic accuracy checks can still
overrun an interval; [full timing distributions and method](docs/GPU_BENCHMARK_20260911.md)
are available before sizing a live system.

VectorWarp also completed configurations that the upstream processor cannot
safely represent: a ±4000 Hz pair took 319.268 ms/CPI on the GPU, while upstream
was excluded because its Doppler scratch allocation is too small. A five-channel
array took 302.140 ms/CPI on the GPU. Those workloads are VectorWarp-only, so
they are capacity results rather than speedups over blah2.

The GPU accelerates clutter FFT/filtering and delay–Doppler processing. Its small
FP64 coefficient solve and the remaining radar stages stay on CPU. [Full method,
settings, deadline counts, accuracy checks, and limitations →](docs/GPU_BENCHMARK_20260911.md)

## Wide Doppler for higher-frequency experiments

VectorWarp processed a recorded 527 MHz signal across **±40 kHz** at 100, 200
and 500 ms CPIs in **87, 171 and 419 ms** on the GPU; a ±20 kHz, one-second CPI
took **769 ms**. This is demonstrated processing capacity, not a claim that
every frame meets its deadline: each GPU result had two periodic CPU accuracy
checks outside its budget.

Higher RF carrier frequencies create larger Doppler shifts for the same motion,
so this headroom can support experiments with higher-frequency illuminators such
as satellite-TV or LEO downlinks. It does not add an RF frontend or establish
support for Starlink, Ku-band IQ, moving-illuminator compensation, link budget,
or aircraft detection. Doppler span is not receiver bandwidth or carrier
frequency. The tradeoff is range coverage: ±40 kHz used 32 delay bins (up to
2.623 km excess path), while ±20 kHz used 64 (up to 6.620 km).

[Wide-Doppler capacity, tails, and experimental context →](docs/GPU_BENCHMARK_20260911.md#wide-doppler-capacity)

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
