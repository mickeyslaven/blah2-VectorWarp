<p><img src="html/favicon/vectorwarp-vw.svg" width="64" height="64" alt="VectorWarp"></p>

# VectorWarp

VectorWarp builds on [blah2](https://github.com/30hours/blah2) with faster CPU and GPU processing, a redesigned browser interface, editable settings, built-in ADS-B integration, and native Linux installation. Set up your radar, watch it run, and record or replay signals from the same interface.

[Install](https://mickeyslaven.github.io/blah2-VectorWarp/#install) · [Set up a receiver](docs/SETUP.md) · [Comparison details](docs/UPSTREAM_COMPARISON.md) · [GPU acceleration](docs/GPU_ACCELERATION.md)

## What you get beyond blah2

- **GPU acceleration:** use a compatible GPU for clutter filtering and delay–Doppler processing, with automatic selection and CPU fallback.
- **Faster without a GPU, too:** less copying, repeated detector work and output conversion; CPU-only processing took up to 26% less time in the matched desktop/laptop tests below.
- **Wider Doppler coverage:** process valid delay/Doppler combinations that exceed the original processor's buffer limits. See the equal-range results below.
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

Using several radar nodes with 3lips? See [3lips setup](docs/3LIPS_SETUP.md), including its separate ADS-B requirements.

## Faster processing in practice

VectorWarp's optional Vulkan GPU path can make a practical radar workload keep
up where regular blah2 falls behind. CPU mode remains available, and Automatic
mode checks accuracy before retaining GPU work. At 200 ms CPI and ±2400 Hz,
these hosts used **about 25% less CPU-only processing time**, or **64–71% less
with GPU acceleration**, than original blah2.

On the older Pavilion CPU alone, the ±800 Hz workload dropped from 205.7 to
167.8 ms. Original blah2 missed all 24 measured 200 ms deadlines; VectorWarp
met all 24 without a GPU.

| Two channels, same IQ and CPU budget | Regular blah2 CPU | VectorWarp CPU | VectorWarp GPU |
| --- | ---: | ---: | ---: |
| Strix, 200 ms CPI, ±800 Hz | 79.5 ms | 65.5 ms | **31.1 ms** |
| RTX 4050 Laptop, 200 ms CPI, ±800 Hz | 230.5 ms | 180.9 ms | **63.6 ms** |
| Pavilion AMD GPU, 200 ms CPI, ±2400 Hz | 306.8 ms | 229.9 ms | **88.1 ms** |

The comparison used actual [30hours/blah2 at `c821bee`](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de)
and VectorWarp on each host, replaying the same recorded signal at its original
rate. Every comparison used 527 MHz, 2.4 MS/s, delays −10…245 (256 bins; 30.604
km maximum excess path), and the stated channel count. Accuracy qualification
runs at startup; accepted GPU frames do not repeat the full CPU calculation.
[Full timing distributions and method](docs/GPU_BENCHMARK_20260911.md)
are available before sizing a live system.

The GPU accelerates clutter FFT/filtering and delay–Doppler processing. Its small
FP64 coefficient solve and the remaining radar stages stay on CPU. [Full method,
settings, deadline counts, accuracy checks, and limitations →](docs/GPU_BENCHMARK_20260911.md)

**Earlier live proof:** at 527 MHz and 2.4 MS/s, a five-channel Kraken array GPU run
at ±800 Hz and 200 ms CPI averaged **95.5 ms**. This is capacity evidence, not
a matched upstream speed comparison. It used the prior version with recurring
CPU accuracy checks; the refreshed measurements above are recorded-IQ replays,
not new live or endurance tests.

## Equal-range Doppler tests

On a Raspberry Pi 4, the CPU workload at 200 ms CPI and ±800 Hz took **798.2 ms** in VectorWarp,
versus **905.1 ms** in original blah2 and **905.9 ms** in Off World Labs' ARM
fork. [Pi comparison and workload limits](docs/PI4_PERFORMANCE_20260911.md)


At this same full range and **±4800 Hz**, Strix GPU processing averaged **67.0 ms
per 200 ms CPI**, or **325.5 ms per one-second CPI**. VectorWarp CPU took 173.8 ms
and 886.0 ms respectively; both modes met all 24 steady deadlines in each case.
Original blah2's Doppler buffer is too small for
these settings; it was not run with unsafe buffer sizes.

The combined efficiency changes also reduced the Strix ±2400 Hz GPU workload
from 69.7 to 44.7 ms versus the preceding VectorWarp version in the same campaign.
[Before/after results and remaining processing costs](docs/GPU_BENCHMARK_20260911.md#remaining-processing-costs)
keep that separate from the upstream comparison. Heavier workloads can still
miss deadlines; the full report includes every tested configuration.

## Install on Linux

Start with [the installation guide](docs/INSTALL.md), then follow [receiver setup](docs/SETUP.md).

Source installation is available now. Planned releases use one package per OS
and architecture with Kraken, USRP and HackRF adapters plus locally buildable
RSPduo support; they are not yet published.
Choose your receiver in the web settings.

- **KrakenSDR Suite V2:** 2–8-channel network input, synthesized or dedicated reference, and combined surveillance maps.
- **USRP (including B210) and dual HackRF:** receiver settings are passed to UHD or libhackrf when processing starts.
- **SDRplay RSPduo:** uses your locally installed SDRplay API; Settings checks installation and service status and links to [SDRplay setup](docs/SDRPLAY_SETUP.md) when needed.

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
