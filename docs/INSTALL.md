# Install VectorWarp

Choose the installation path for your operating system:

- **macOS:** [Homebrew from the local port checkout](#macos-with-homebrew).
  Apple Silicon is tested on M2; Intel remains experimental and unverified.
- **Linux:** [Published DEB/RPM packages](#install-a-package) for 64-bit Linux
  with systemd: x86-64 (amd64 / x86_64) or ARM64 (arm64 / aarch64).

## macOS with Homebrew

With Homebrew and Xcode Command Line Tools installed, run from the root of a
checkout containing the macOS port:

```sh
script/package-homebrew-local.sh build/homebrew-local --install-tap
brew trust vectorwarp/local
brew install --build-from-source vectorwarp/local/vectorwarp
vectorwarp
```

The formula builds VectorWarp, its local USB Kraken companion, the UHD/HackRF
adapters and optional Vulkan/MoltenVK processing with CPU fallback. It does not
include or download the proprietary SDRplay SDK. `vectorwarp` opens Settings
without starting radar; configure your receiver or replay file, then choose
**Save & Restart**.

Follow the [Homebrew guide](MACOS_HOMEBREW.md) for installation checks, per-user
services, updates and removal. The [Mac guide](MACOS.md) covers receiver setup,
configuration paths and direct source builds. The package is locally tested
through revision 17 on Apple M2. Public Mac formulas and bottles are not
available yet; the commands require this port checkout. The
[public tap workflow](HOMEBREW_PUBLISHING.md) is prepared to publish formulas
after merges to `main` pass its installation checks. Public v0.1.7 release assets
remain Linux-only.

## Linux installation

Use the [package download and installation page](https://mickeyslaven.github.io/blah2-VectorWarp/#install)
for current downloads and APT/DNF setup. The same package-manager steps are
included below; [building from source](#build-from-source) is a separate route.

## Install a package

### 1. Install VectorWarp

Copy the command for your OS. It installs the prerequisites, adds our signed
repository, installs VectorWarp, and opens the web interface. Enter your
administrator password if prompted. The command stops if a step fails.

```bash
# Ubuntu or Debian
sudo apt update && sudo apt install -y curl gnupg && curl --fail --location --proto '=https' --tlsv1.2 https://mickeyslaven.github.io/blah2-VectorWarp/install.sh --output vectorwarp-install.sh && sudo bash vectorwarp-install.sh --repo-only && sudo apt update && sudo apt install -y vectorwarp && vectorwarp
```

```bash
# Fedora
sudo dnf install -y curl gnupg2 && curl --fail --location --proto '=https' --tlsv1.2 https://mickeyslaven.github.io/blah2-VectorWarp/install.sh --output vectorwarp-install.sh && sudo bash vectorwarp-install.sh --repo-only && sudo dnf install -y vectorwarp && vectorwarp
```

The [installer source](../script/install-release.sh) is available to read separately.
The commands download over HTTPS and verify the repository's pinned signing key.
They open only the web interface; configure the receiver before starting radar.

For scripted installations, the installer also accepts `--start-web` instead
of `--repo-only` and the following package-manager commands. It installs the
package, enables the web API at boot and starts only that service directly.
On upgrades, package hooks may restore previously running radar.

### 2. Configure and start radar

Open **Settings**, select and configure the receiver, then choose **Save &
Restart**. This saves the configuration and starts radar, including on the first run.
**Save for later** saves without starting it; use `vectorwarp start` later to start the
full stack with saved settings.

### 3. Use controls and help

The following launcher behavior is included in version 0.1.7 and newer.
`vectorwarp` opens the configured web address (or prints it over SSH) without
starting radar. `vectorwarp --help` lists its fixed actions:

| Command | What it does |
| --- | --- |
| `vectorwarp` / `vectorwarp open` | Open the web interface without starting radar. |
| `vectorwarp start` | Start the full stack with saved settings. |
| `vectorwarp stop` | Stop the full stack, including the web interface. |
| `vectorwarp restart` | Stop and start the full stack with saved settings. |
| `vectorwarp status` | Show service status. |
| `vectorwarp logs` | Show recent logs. |
| `vectorwarp version` | Show the installed version. |
| `vectorwarp help` | Show all commands and examples. |

`open` (the default) starts only the web API. `start` brings up the full
VectorWarp stack: receiver helper, web API, and checked radar processor using
saved settings. If radar is active but the web page is down, `start` repairs
the web and helper without interrupting radar; use `restart` to apply changed
settings. `stop` safely drains privileged receiver work before stopping the
VectorWarp processor, web API, helper and socket; `restart` stops and starts
the full stack in order. `version` reports the installed package version. A
pending package upgrade, receiver build or restart can make a command refuse
until it is safe to retry. Neither these commands nor package upgrades stop
shared Kraken Suite, SDRplay API or other vendor services. Checked receiver
startup can still start an approved installed receiver service when the
selected profile requires it.
Check `status` or `logs` after a request. A service-changing command may ask
for your administrator password. If your user cannot read the system journal,
use `sudo vectorwarp logs`. Packages older than 0.1.7 do not contain the
launcher; update them with the package-manager commands below before using it.

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

If an earlier installation completed but the page does not load, start and
inspect only the web API:

```bash
sudo systemctl enable --now vectorwarp-api.service
sudo systemctl status vectorwarp-api.service --no-pager
```

On an upgrade to version 0.1.7 or newer, the package pauses its own services before replacing files,
waits for privileged receiver work to finish,
and restarts only VectorWarp services that were running beforehand. A stopped
processor stays stopped. Pending receiver-management approvals expire; review
them again after the upgrade. An active local SDRplay build or Save & Restart
blocks the upgrade until it finishes. No Kraken Suite, SDRplay API or other
vendor software is upgraded by these hooks.
If you use a locally built RSPduo adapter, a changed VectorWarp core can make
that adapter stale. The package does not build it automatically: check
**Receiver setup → Build SDRplay support** in Settings and rebuild if prompted,
then use **Save & Restart**. Restarting the previously active processor unit
does not guarantee that its receiver became ready.

If preflight reports that the broker cannot be safely drained (for example on
an unsupported cgroup layout or an overridden broker unit), no new package
files are unpacked; follow the printed status guidance and retry after the
receiver action finishes. If activation fails after unpack, processing remains
stopped and the package reports a failure. Inspect the four VectorWarp unit
statuses named in that error before starting only the services you intend.
If systemd itself cannot reload, the package retains its upgrade marker and
the launcher refuses service actions; repair systemd first, then rerun
`sudo systemctl daemon-reload`. On APT systems, then run
`sudo dpkg --configure vectorwarp`; its post-install hook will consume the
marker and restore the prior active services. On Fedora, run
`sudo /opt/vectorwarp/libexec/vectorwarp-activate-upgrade` once instead.
If activation itself failed, its marker was consumed; inspect the printed
previously-active state and service errors. An APT reconfiguration will mark
the package configured without replaying that failed start. Start only the
services you choose after fixing the error.

### Manual package installation

Use the current [installation page](https://mickeyslaven.github.io/blah2-VectorWarp/#install)
to select one package matching the operating system version and architecture.
Install a downloaded local package with the distribution package manager so its
dependencies resolve; do not use `dpkg` alone or mix libraries from another
release. Replace `matching.deb` or `matching.rpm` with the downloaded filename:

```bash
# Ubuntu, Debian, or DragonOS using its matching Ubuntu base
sudo apt install ./matching.deb
```

```bash
# Fedora
sudo dnf install ./matching.rpm
```

## Supported systems

| System | Version or base | Architecture | Current package selection |
| --- | --- | --- | --- |
| Ubuntu | 22.04, 24.04, or 26.04 | x86-64 or ARM64 | [Installation page](https://mickeyslaven.github.io/blah2-VectorWarp/#install) |
| Debian | 13 (Trixie) | x86-64 or ARM64 | [Installation page](https://mickeyslaven.github.io/blah2-VectorWarp/#install) |
| Fedora | 44 | x86-64 or ARM64 | [Installation page](https://mickeyslaven.github.io/blah2-VectorWarp/#install) |
| [DragonOS](DRAGONOS.md) | Matching Ubuntu base listed above | x86-64 or ARM64 | [Use `/etc/os-release` metadata](DRAGONOS.md) |
| Raspberry Pi OS | Trixie, 64-bit | ARM64 | [Installation page](https://mickeyslaven.github.io/blah2-VectorWarp/#install) |

Here, x86-64 means `amd64` or `x86_64`; ARM64 means `arm64` or `aarch64`.

A Raspberry Pi can also use a listed 64-bit Fedora, Debian or Ubuntu release;
follow that operating system's instructions. These are build/package targets,
not a guarantee that every radar workload will run in real time on every device.
DragonOS and Raspberry Pi OS package selection has fixture coverage, not a
clean-host installation claim. A 32-bit operating system is not supported.

<a id="build-from-source"></a>

## Build from source

These source-build commands target Linux. For macOS, use the
[Mac source-build guide](MACOS.md#build-and-run) or Homebrew above.
Use this route for development or a system outside the Linux package targets.

### 1. Install build dependencies

Install [Node.js](https://nodejs.org/en/download) 22 or later with npm, then
the build dependencies below. On Ubuntu or Debian:

```bash
sudo apt update && \
sudo apt install build-essential cmake ninja-build git curl tar zip unzip pkg-config sudo python3 python3-apt \
  libfftw3-dev libarmadillo-dev libuhd-dev uhd-host libboost-dev libhackrf-dev libusb-1.0-0-dev
```

On Fedora:

```bash
sudo dnf install gcc-c++ cmake make ninja-build git curl tar zip unzip pkgconf-pkg-config sudo python3 python3-libdnf5 python3-rpm \
  fftw-devel armadillo-devel uhd-devel boost-devel hackrf-devel libusb1-devel
```

The native installer requires Node.js 22 or newer at `/usr/bin/node`, not only
inside your shell's version manager. Check it before building:

```bash
/usr/bin/node --version
npm --version
```

If Node is missing or older than 22, finish a system-wide Node installation
before continuing. The official download page also offers shell-only version
managers; those do not satisfy the `/usr/bin/node` requirement by themselves.

### GPU build dependencies

For GPU support, also install the development packages for your system:

```bash
# Ubuntu or Debian
sudo apt install libvulkan-dev glslang-dev glslang-tools
```

```bash
# Fedora
sudo dnf install vulkan-loader-devel glslang-devel vulkan-tools
```

Install your GPU's Vulkan driver before using acceleration. `--gpu auto` does
not install drivers or development packages.
If the GPU module was omitted from a build, installing a driver alone is not
enough: rebuild and reinstall VectorWarp with those dependencies present.

### 2. Choose the receivers to include

| `--backend` | Live receivers included | Additional receiver software |
| --- | --- | --- |
| `kraken` | Kraken | Local or remote KrakenSDR Suite V2 |
| `usrp` | USRP and Kraken | UHD 4.1+ |
| `hackrf` | Dual HackRF and Kraken | libhackrf |
| `rspduo` | RSPduo and Kraken | SDRplay Hardware API 3.15 and its development headers |
| `all` (default) | Kraken, USRP, dual HackRF; locally buildable RSPduo | UHD and libhackrf; SDRplay API only if using RSPduo |

These are source-build choices, not separate release products. UHD and HackRF
development packages are included above. `all` does not need the SDRplay SDK at
build time: it includes our small adapter source kit for local compilation from
Settings. Before choosing the precompiled `rspduo` source-build option,
install and license the [SDRplay API yourself](SDRPLAY_SETUP.md).
Set `BLAH2_SDRPLAY_INCLUDE_DIR` and `BLAH2_SDRPLAY_LIBRARY` if its headers and
library are outside the standard paths. VectorWarp never downloads the SDK or
accepts its license.

### 3. Build and install

Set `VW_BACKEND` below to your choice. Keep the same value for both build
commands; omitting `--backend` defaults to `all` without downloading an SDRplay SDK.

```bash
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git && \
cd blah2-VectorWarp && \
VW_BACKEND=all && \
script/build-native.sh --preflight --backend "$VW_BACKEND" --gpu auto && \
script/build-native.sh --backend "$VW_BACKEND" --gpu auto && \
sudo script/install-native.sh --preflight && \
sudo script/install-native.sh
```

The build creates `build/native/artifact`. Use `--gpu off` in both build
commands for a CPU-only build. Installation preserves an existing
`/etc/vectorwarp/config.yml` and does not enable or start VectorWarp services.
A first RSPduo-enabled installation can start an already-installed standard
SDRplay API service; upgrades do not. See [SDRplay setup](SDRPLAY_SETUP.md).

<a id="start-the-interface-and-configure-vectorwarp"></a>

### 4. Open the interface and configure your receiver

Open the web interface without starting radar processing:

```bash
vectorwarp
```

The default launcher action starts only the web API when needed and prints its
configured address; on a local desktop it also opens the browser. The default address on
the server is `http://localhost:3000/`. From another device, use
`http://<server-IP>:3000/`, replacing `<server-IP>` with the Linux machine's
address. Do not use `localhost` from your phone or another computer.
The default configuration listens on all network interfaces. The UI has no
login: keep it on a trusted LAN/VPN or behind an authenticated gateway.
If the launcher reports a web-service error, an administrator can inspect and
retry that unit directly:

```bash
sudo systemctl status vectorwarp-api.service --no-pager
sudo systemctl start vectorwarp-api.service
```

This starts only the API, not radar processing.
For a native source installation using a custom prefix, the full-stack
`vectorwarp stop` and `vectorwarp restart` actions refuse rather than assume
the standard package service layout. After receiver actions finish, an
administrator must inspect that installation's units and stop them manually.

In **Settings**, select a receiver profile; supply its endpoint or device
details; set frequency, sample rate, site coordinates, and a writable recording
directory; then correct any validation errors. Install and configure
KrakenSDR Suite separately before using its endpoint.
Follow the [receiver-specific steps](SETUP.md) for Kraken, RSPduo, USRP or HackRF.

For SDRplay RSPduo, obtain and license SDRplay's vendor Hardware API yourself.
VectorWarp never downloads it or accepts its license. On a first real native or
package installation only, VectorWarp may prepare an already-installed local
vendor service when the local RSPduo kit and policy allow it; it
does not enable it at boot or start VectorWarp itself. If preparation cannot be
verified, use the official [SDRplay Hardware API page](https://sdrplay.com/hardware-api/),
then recheck in Settings. With the unified package, choose **Build SDRplay support**
after installing the API and its headers. This compiles only our adapter; it does
not start radar. Then use **Save & Restart**. See
[SDRplay setup](SDRPLAY_SETUP.md).

Choose **Save & Restart** (or **Apply & Restart** for previously saved changes)
to apply the settings and start processing. **Save for later**, when offered,
writes the configuration without applying receiver changes or starting radar.
The page shows restart progress and any startup error. Software discovery does not prove
that the receiver is correctly wired or receiving a coherent signal.

To start only the web interface automatically after future reboots, enable its
service. Enable radar processing at boot separately, only after confirming the
receiver configuration works and choosing that behavior deliberately:

```bash
sudo systemctl enable vectorwarp-api.service
# Optional: also start radar processing after reboot
sudo systemctl enable vectorwarp-processor.service
```

For ordinary status and recent web/radar logs, use:

```bash
vectorwarp status
vectorwarp logs
```

Administrators can still use `systemctl status` for detailed unit-level repair.

## Optional setup

<a id="raspberry-pi-and-gpu-acceleration"></a>

### Raspberry Pi GPU

Use a 64-bit operating system and the same source route. GPU acceleration is
optional. After installation, inspect the native driver status:

```bash
/opt/vectorwarp/libexec/vectorwarp-gpu-setup --status
```

The separate administrator command can offer the operating system's Mesa
packages when needed:

```bash
sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --install-driver
```

It does not start or restart radar processing. See [Pi GPU setup](PI_GPU_SETUP.md)
for driver and account-access details.

<a id="future-package-route"></a>

### Package notes

The repository installer and direct-package links above cover Ubuntu 22.04,
24.04, and 26.04; Debian 13; Fedora 44; matching DragonOS bases; and 64-bit
Raspberry Pi OS Trixie. See [DragonOS](DRAGONOS.md) for Ubuntu-base selection.
