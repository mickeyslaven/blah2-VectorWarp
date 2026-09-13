# Install VectorWarp

These instructions install VectorWarp on 64-bit Linux with systemd:
x86-64 (amd64 / x86_64) or ARM64 (arm64 / aarch64).
Build from source for now; the first signed DEB/RPM
release is not yet published.

## Supported systems

| System | Version or base | Architecture |
| --- | --- | --- |
| Ubuntu | 22.04, 24.04, 26.04 | x86-64 or ARM64 |
| Debian | 13 (Trixie) | x86-64 or ARM64 |
| Fedora | 44 | x86-64 or ARM64 |
| [DragonOS](DRAGONOS.md) | Matching Ubuntu base listed above | x86-64 or ARM64 |
| Raspberry Pi OS | Trixie, 64-bit | ARM64 |

A Raspberry Pi can also use a listed 64-bit Fedora, Debian or Ubuntu release;
follow that operating system's instructions. These are build/package targets,
not a guarantee that every radar workload will run in real time on every device.
DragonOS and Raspberry Pi OS package selection has fixture coverage, not a
clean-host installation claim. A 32-bit operating system is not supported.

<a id="build-from-source"></a>

## 1. Install build dependencies

Install [Node.js](https://nodejs.org/en/download) 22 or later with npm, then
the build dependencies below. On Ubuntu or Debian:

```bash
sudo apt update
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

## 2. Choose the receivers to include

| `--backend` | Live receivers included | Additional receiver software |
| --- | --- | --- |
| `kraken` | Kraken | Local or remote KrakenSDR Suite V2 |
| `usrp` | USRP and Kraken | UHD 4.1+ |
| `hackrf` | Dual HackRF and Kraken | libhackrf |
| `rspduo` | RSPduo and Kraken | SDRplay Hardware API 3.15 and its development headers |
| `all` | All four | All of the above |

These are source-build choices, not separate release products. UHD and HackRF
development packages are included above. Before choosing `rspduo` or `all`,
install and license the [SDRplay API yourself](SDRPLAY_SETUP.md).
Set `BLAH2_SDRPLAY_INCLUDE_DIR` and `BLAH2_SDRPLAY_LIBRARY` if its headers and
library are outside the standard paths. VectorWarp never downloads the SDK or
accepts its license.

## 3. Build and install

Set `VW_BACKEND` below to your choice. Keep the same value for both build
commands; omitting `--backend` defaults to `all` and requires the SDRplay SDK.

```bash
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
cd blah2-VectorWarp
VW_BACKEND=kraken
script/build-native.sh --preflight --backend "$VW_BACKEND" --gpu auto
script/build-native.sh --backend "$VW_BACKEND" --gpu auto
sudo script/install-native.sh --preflight
sudo script/install-native.sh
```

The build creates `build/native/artifact`. Use `--gpu off` in both build
commands for a CPU-only build. Installation preserves an existing
`/etc/vectorwarp/config.yml` and does not enable or start VectorWarp services.
A first RSPduo-enabled installation can start an already-installed standard
SDRplay API service; upgrades do not. See [SDRplay setup](SDRPLAY_SETUP.md).

<a id="start-the-interface-and-configure-vectorwarp"></a>

## 4. Open the interface and configure your receiver

Start only the web API first:

```bash
sudo systemctl start vectorwarp-api.service
```

On the server, open `http://localhost:3000/`. From another device, use
`http://<server-IP>:3000/`, replacing `<server-IP>` with the Linux machine's
address. Do not use `localhost` from your phone or another computer.
The default configuration listens on all network interfaces. The UI has no
login: keep it on a trusted LAN/VPN or behind an authenticated gateway.

In **Settings**, select a receiver profile; supply its endpoint or device
details; set frequency, sample rate, site coordinates, and a writable recording
directory; then correct any validation errors. Install and configure
KrakenSDR Suite separately before using its endpoint.
Follow the [receiver-specific steps](SETUP.md) for Kraken, RSPduo, USRP or HackRF.

For SDRplay RSPduo, obtain and license SDRplay's vendor Hardware API yourself.
VectorWarp never downloads it or accepts its license. On a first real native or
package installation only, VectorWarp may prepare an already-installed local
vendor service when the installed RSPduo adapter and local policy allow it; it
does not enable it at boot or start VectorWarp itself. If preparation cannot be
verified, use the official [SDRplay Hardware API page](https://sdrplay.com/hardware-api/),
then recheck in Settings. Building an RSPduo adapter separately requires the
licensed API 3.15 headers and library described above. See
[SDRplay setup](SDRPLAY_SETUP.md).

Choose **Save & Restart** (or **Apply & Restart** for previously saved changes)
to apply the settings and start processing. **Save for later**, when offered,
writes the configuration without applying receiver changes or starting radar.
The page shows restart progress and any startup error. Software discovery does not prove
that the receiver is correctly wired or receiving a coherent signal.

To start VectorWarp automatically after future reboots, enable the services
after confirming the configuration works:

```bash
sudo systemctl enable vectorwarp-api.service vectorwarp-processor.service
```

Check either service with:

```bash
systemctl status vectorwarp-api.service vectorwarp-processor.service
```

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

### Package availability

The repository contains package-install tooling for Ubuntu 22.04/24.04/26.04,
Debian 13, Fedora 44, matching DragonOS bases, and 64-bit Raspberry Pi OS
Trixie. Packages will be linked here when published. For now, use the source
commands above; the planned APT/DNF bootstrap is not an available installer.
See [DragonOS](DRAGONOS.md) for supported Ubuntu-base selection.
