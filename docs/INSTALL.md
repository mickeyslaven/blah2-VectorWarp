# Install VectorWarp

Use the signed Linux packages or Homebrew for a normal installation. Source
builds and standalone packages are available below.

## Linux

### Ubuntu or Debian

```bash
sudo apt update && sudo apt install -y curl gnupg
curl --fail --location --proto '=https' --tlsv1.2 \
  https://mickeyslaven.github.io/blah2-VectorWarp/install.sh \
  --output vectorwarp-install.sh
sudo bash vectorwarp-install.sh --repo-only
sudo apt update && sudo apt install -y vectorwarp
vectorwarp
```

### Fedora

```bash
sudo dnf install -y curl gnupg2
curl --fail --location --proto '=https' --tlsv1.2 \
  https://mickeyslaven.github.io/blah2-VectorWarp/install.sh \
  --output vectorwarp-install.sh
sudo bash vectorwarp-install.sh --repo-only
sudo dnf install -y vectorwarp
vectorwarp
```

The installer adds the VectorWarp package repository and signing key. The last
command opens the browser interface.

## macOS

Install Homebrew and the Xcode Command Line Tools, then run:

```sh
brew tap mickeyslaven/vectorwarp
brew trust mickeyslaven/vectorwarp
brew install vectorwarp
vectorwarp
```

Homebrew builds VectorWarp and installs its local KrakenSDR companion, UHD,
HackRF, and GPU dependencies. The Kraken companion is a dependency of
VectorWarp and does not need to be installed or upgraded separately.

## Complete setup

`vectorwarp` opens **Settings**. Select **Start setup** and complete Receiver,
Tuning, Sites, Radar, Data, and Review. **Finish & Start radar** saves the
configuration, applies it to the receiver, and starts processing.

See [Set up VectorWarp](SETUP.md) for the complete guided setup, receiver,
array, ADS-B, recording, and replay instructions.

## Update

### Ubuntu or Debian

```bash
sudo apt update
sudo apt install vectorwarp
```

### Fedora

```bash
sudo dnf upgrade --refresh vectorwarp
```

### macOS

```sh
brew update
brew upgrade vectorwarp
vectorwarp restart
```

When VectorWarp runs as a Homebrew service, restart it with:

```sh
brew services restart vectorwarp
```

## Commands

| Command | Action |
| --- | --- |
| `vectorwarp` or `vectorwarp open` | Open the browser interface. |
| `vectorwarp start` | Start the receiver, API, and radar processor. |
| `vectorwarp stop` | Stop VectorWarp. |
| `vectorwarp restart` | Restart VectorWarp with the saved settings. |
| `vectorwarp status` | Show service status. |
| `vectorwarp logs` | Show recent logs. |
| `vectorwarp version` | Show the installed Linux package version. |
| `vectorwarp help` | List available commands. |

On macOS, show the installed version with:

```sh
brew list --versions vectorwarp
```

## Open VectorWarp from another device

Open this address in a browser on the same network:

```text
http://<radar-computer-address>:3000/
```

Use the radar computer's hostname or IP address in place of
`<radar-computer-address>`.

## Supported systems

| System | Version or base | Architecture | Install method |
| --- | --- | --- | --- |
| Ubuntu | 22.04, 24.04, or 26.04 | x86-64 or ARM64 | APT |
| Debian | 12 Bookworm | ARM64 | APT |
| Debian | 13 Trixie | x86-64 or ARM64 | APT |
| Fedora | 44 | x86-64 or ARM64 | DNF |
| DragonOS | Matching supported Ubuntu base | x86-64 or ARM64 | APT |
| macOS | Current supported release | Apple Silicon | Homebrew |

The [package page](https://mickeyslaven.github.io/blah2-VectorWarp/#install)
selects the matching download for each supported Linux system.

## Manual package installation

Download the matching package from the
[package page](https://mickeyslaven.github.io/blah2-VectorWarp/#install), then
install it with the system package manager.

```bash
# Ubuntu, Debian, or DragonOS
sudo apt install ./vectorwarp-package.deb
```

```bash
# Fedora
sudo dnf install ./vectorwarp-package.rpm
```

When a signed macOS PKG is listed on the package page, open it and follow the
installer. The application is installed at `/Applications/VectorWarp.app`.

## Build from source

Source builds support Linux systems with Node.js 22 or newer, CMake, a C++
compiler, and the receiver libraries selected below.

### 1. Install Linux build dependencies

Ubuntu or Debian:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git curl tar zip unzip \
  pkg-config sudo python3 python3-apt libfftw3-dev libarmadillo-dev \
  libuhd-dev uhd-host libboost-dev libhackrf-dev libusb-1.0-0-dev \
  libeigen3-dev libssl-dev zlib1g-dev patch
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake make ninja-build git curl tar zip unzip \
  pkgconf-pkg-config sudo python3 python3-libdnf5 python3-rpm fftw-devel \
  armadillo-devel uhd-devel boost-devel hackrf-devel libusb1-devel \
  eigen3-devel openssl-devel zlib-devel patch
```

Install Node.js 22 or newer, then check:

```bash
/usr/bin/node --version
npm --version
```

For GPU processing, install Vulkan development tools:

```bash
# Ubuntu or Debian
sudo apt install libvulkan-dev glslang-dev glslang-tools

# Fedora
sudo dnf install vulkan-loader-devel glslang-devel vulkan-tools
```

### 2. Choose receiver support

| Backend | Included receivers |
| --- | --- |
| `all` | KrakenSDR, USRP/UHD, dual HackRF, and the local RSPduo adapter kit |
| `kraken` | KrakenSDR |
| `usrp` | USRP/UHD and KrakenSDR |
| `hackrf` | Dual HackRF and KrakenSDR |
| `rspduo` | SDRplay RSPduo and KrakenSDR |

For RSPduo, install the SDRplay Hardware API before building the adapter. See
[SDRplay setup](SDRPLAY_SETUP.md).

### 3. Build and install

```bash
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
cd blah2-VectorWarp
VW_BACKEND=all
script/build-native.sh --preflight --backend "$VW_BACKEND" --gpu auto
script/build-native.sh --backend "$VW_BACKEND" --gpu auto
sudo script/install-native.sh --preflight
sudo script/install-native.sh
vectorwarp
```

For a CPU-only build, replace `--gpu auto` with `--gpu off` in both build
commands.

For macOS source development, see the [macOS build guide](MACOS.md#build-and-run).

## Remove VectorWarp

Ubuntu or Debian:

```bash
sudo apt remove vectorwarp
```

Fedora:

```bash
sudo dnf remove vectorwarp
```

macOS:

```sh
brew services stop vectorwarp
vectorwarp stop
brew uninstall vectorwarp
```

## Other installation guides

- [SDRplay RSPduo](SDRPLAY_SETUP.md)
- [DragonOS](DRAGONOS.md)
- [Raspberry Pi 4](PI4_GUIDE.md)
