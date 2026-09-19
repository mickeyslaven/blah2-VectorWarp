<p><img src="html/favicon/vectorwarp-vw.svg" width="64" height="64" alt="VectorWarp"></p>

# VectorWarp

> **THIS IS A DEVELOPMENT BUILD. EXPECT BUGS AND REPORT VIA GITHUB ISSUES PLEASE AND THANK YOU!**

VectorWarp builds on [blah2](https://github.com/30hours/blah2) with faster CPU and GPU processing, a redesigned browser interface, editable settings, built-in ADS-B integration, and native Linux installation. Set up your radar, watch it run, and record or replay signals from the same interface.

[Install](https://mickeyslaven.github.io/blah2-VectorWarp/#install) · [Set up a receiver](docs/SETUP.md) · [Comparison details](docs/UPSTREAM_COMPARISON.md) · [GPU acceleration](docs/GPU_ACCELERATION.md) · [Pi 4 guide](docs/PI4_GUIDE.md)

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

A separate Pi 4 replay with a newer Mesa driver reduced processing from
**797.2 ms CPU-only to 578.1 ms with GPU Automatic**—another **27.5% less time**
on that workload. The driver was loaded only for the test; this still did not
meet the 200 ms deadline. [Pi GPU results and driver update instructions](docs/PI_GPU_SETUP.md#pi-4-driver-diagnostic)


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

Start with [downloads and APT/DNF setup](https://mickeyslaven.github.io/blah2-VectorWarp/#install). The [full installation guide](docs/INSTALL.md) also covers source builds.

### 1. Install VectorWarp

There is one package per OS and architecture, with Kraken, USRP and HackRF
adapters plus locally buildable RSPduo support. Packages cover the listed
64-bit Ubuntu, Debian and Fedora releases. Copy the command for your OS. It
installs the prerequisites, adds our signed repository, installs VectorWarp,
and opens the web interface. Enter your administrator password if prompted.

```bash
# Ubuntu or Debian
sudo apt update && sudo apt install -y curl gnupg && curl --fail --location --proto '=https' --tlsv1.2 https://mickeyslaven.github.io/blah2-VectorWarp/install.sh --output vectorwarp-install.sh && sudo bash vectorwarp-install.sh --repo-only && sudo apt update && sudo apt install -y vectorwarp && vectorwarp
```

```bash
# Fedora
sudo dnf install -y curl gnupg2 && curl --fail --location --proto '=https' --tlsv1.2 https://mickeyslaven.github.io/blah2-VectorWarp/install.sh --output vectorwarp-install.sh && sudo bash vectorwarp-install.sh --repo-only && sudo dnf install -y vectorwarp && vectorwarp
```

The [installer source](script/install-release.sh) is available to read separately.

### 2. Configure and start radar

Open **Settings**, select and configure the receiver, then choose **Save &
Restart**. This saves the first configuration and starts radar; no separate
`vectorwarp start` is needed. See [receiver setup](docs/SETUP.md) for receiver
details.

### 3. Use controls and help

`vectorwarp` (or `vectorwarp open`) opens only the web interface.
`vectorwarp start` starts the full VectorWarp stack; `vectorwarp stop` stops it,
including the web page; and `vectorwarp restart` restarts it in order.
Use `vectorwarp status`, `vectorwarp logs` and `vectorwarp version` to check it,
or `vectorwarp help` for all commands. These commands and upgrades never stop shared Kraken Suite or SDRplay
services.

### 4. Update

Keep the configured repository; do not rerun the installer. Choose one block:

```bash
# Ubuntu or Debian
sudo apt update && sudo apt install vectorwarp
```

```bash
# Fedora
sudo dnf upgrade --refresh vectorwarp
```

Upgrades to version 0.1.7 or newer restore only the VectorWarp services that
were running; intentionally stopped radar stays stopped.
Finish receiver setup/build actions before updating. If the upgrade cannot
stop a helper safely, it reports the problem before unpacking new files.

## SDR support

- **KrakenSDR Suite V2:** 2–8-channel network input, synthesized or dedicated reference, and combined surveillance maps.
- **USRP (including B210) and dual HackRF:** receiver settings are passed to UHD or libhackrf when processing starts.
- **SDRplay RSPduo:** uses your locally installed SDRplay API; Settings checks installation and service status and links to [SDRplay setup](docs/SDRPLAY_SETUP.md) when needed.

The Kraken multi-channel foundation is also offered to blah2 in
[our upstream PR #45](https://github.com/30hours/blah2/pull/45).
VectorWarp adds the interface, integration, and processing improvements above;
the PR remains separate.

## OS support

The repository installer chooses the matching signed APT or DNF repository.
For a direct download, use the current, auto-generated
[installation page](https://mickeyslaven.github.io/blah2-VectorWarp/#install),
which lists only packages in the selected release. Source installation remains
available for development or unsupported systems.

| Operating system | Versions | Architectures | Package |
| --- | --- | --- | --- |
| Ubuntu | 22.04, 24.04, 26.04 | x86-64, ARM64 | [DEB downloads](https://mickeyslaven.github.io/blah2-VectorWarp/#install) |
| Debian | 13 (Trixie) | x86-64, ARM64 | [DEB downloads](https://mickeyslaven.github.io/blah2-VectorWarp/#install) |
| Fedora | 44 | x86-64, ARM64 | [RPM downloads](https://mickeyslaven.github.io/blah2-VectorWarp/#install) |
| DragonOS | Matching Ubuntu base | x86-64, ARM64 | [Use `/etc/os-release` metadata](docs/DRAGONOS.md) |
| Raspberry Pi OS (legacy Pi route, deprecated) | Trixie, 64-bit | ARM64 | [Existing Debian 13 ARM64 DEB](https://mickeyslaven.github.io/blah2-VectorWarp/#install) |

Here, x86-64 means `amd64` or `x86_64`; ARM64 means `arm64` or `aarch64`.

**New Pi distribution:** a headless Raspberry Pi 4 microSD image based on
**Raspberry Pi OS Lite 64-bit Bookworm**, with Wi-Fi and SSH setup through
Raspberry Pi Imager. It replaces the deprecated Pi installation route above
once qualified and published. **Raspberry Pi 5 is future work.** Neither image
is currently published; existing ARM64 packages do not imply a qualified Pi
image. See the [Pi 4 guide](docs/PI4_GUIDE.md) and
[Pi image packaging plan](docs/PI_IMAGE_PACKAGING.md).

## Credits and license

Thanks to [30hours](https://github.com/30hours/blah2) for the original radar
engine. Comparisons use [blah2 at c821bee](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de).
ADS-B is used for display and evaluation, not radar detection or tracking input.
[MIT license](LICENSE)
