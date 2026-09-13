# Install VectorWarp

These instructions install VectorWarp on 64-bit Linux with systemd:
x86-64 (amd64 / x86_64) or ARM64 (arm64 / aarch64).
Build from source for now; the first signed DEB/RPM
release is not yet published.

## Build from source

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

For GPU support, install the [Vulkan build dependencies](SETUP.md#build-from-source)
and your GPU's driver before building. `--gpu auto` does not install them.
If the GPU module was omitted from a build, installing a driver alone is not
enough: rebuild and reinstall VectorWarp with those dependencies present.

Build and install VectorWarp:

```bash
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
cd blah2-VectorWarp
script/build-native.sh --preflight --backend kraken --gpu auto
script/build-native.sh --backend kraken --gpu auto
sudo script/install-native.sh --preflight
sudo script/install-native.sh
```

Installation preserves an existing `/etc/vectorwarp/config.yml` and installs
systemd units without enabling or starting them.

The command above builds the Kraken route. Select `--backend usrp`, `hackrf`,
or `rspduo` for those source routes instead. `--backend all` builds the four
adapters together, but RSPduo requires a locally licensed SDRplay API 3.15
development installation. Set `BLAH2_SDRPLAY_INCLUDE_DIR` and
`BLAH2_SDRPLAY_LIBRARY` when its headers and library are not in standard paths.
Use the same backend for both build commands. See the
[receiver build options](SETUP.md#build-from-source).

## Start the interface and configure VectorWarp

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

For SDRplay RSPduo, install and start SDRplay's vendor Hardware API yourself.
VectorWarp provides detection and guidance, not the vendor software. See
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

## Raspberry Pi and GPU acceleration

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

## Future package route

The repository contains package-install tooling for Ubuntu 22.04/24.04/26.04,
Debian 13, Fedora 44, matching DragonOS bases, and 64-bit Raspberry Pi OS
Trixie. Packages will be linked here when published. For now, use the source
commands above; the planned APT/DNF bootstrap is not an available installer.
