# Install VectorWarp

These instructions install VectorWarp on 64-bit Linux with systemd:
x86-64 (amd64 / x86_64) or ARM64 (arm64 / aarch64).

Use the [package download and installation page](https://mickeyslaven.github.io/blah2-VectorWarp/#install)
for current downloads and APT/DNF setup. The same package-manager steps are
included below; [building from source](#build-from-source) is a separate route.

## Install a package

Download the repository installer over HTTPS and inspect it before running it:
it requires `curl` and GnuPG (`gnupg` on Ubuntu/Debian, `gnupg2` on Fedora).
Install either missing tool from the normal distribution repository first.

```bash
curl --fail --location --proto '=https' --tlsv1.2 \
  https://mickeyslaven.github.io/blah2-VectorWarp/install.sh --output vectorwarp-install.sh
less vectorwarp-install.sh
```

On Ubuntu or Debian, add the matching signed APT repository, install the
package, and start only the browser interface in one command:

```bash
sudo bash vectorwarp-install.sh --start-web
```

On Fedora:

```bash
sudo bash vectorwarp-install.sh --start-web
```

`--start-web` enables the web API at boot and starts only that service; it does
not enable radar processing. Run `vectorwarp` to open the configured web
address (or print it over SSH), then configure a receiver in Settings.
`vectorwarp --help` lists the fixed start, stop, restart, status and log actions.
For the usual command-line controls:

```bash
vectorwarp start
vectorwarp stop
vectorwarp status
```

If an earlier installation completed but the page does not load, start and
inspect only the web API:

```bash
sudo systemctl enable --now vectorwarp-api.service
sudo systemctl status vectorwarp-api.service --no-pager
```

To update, use `sudo apt update && sudo apt install --only-upgrade vectorwarp`
on APT or `sudo dnf upgrade vectorwarp` on Fedora. The package pauses its own
services before replacing files, waits for privileged receiver work to finish,
and restarts only VectorWarp services that were running beforehand. A stopped
processor stays stopped. Pending receiver-management approvals expire; review
them again after the upgrade. An active local SDRplay build or Save & Restart
blocks the upgrade until it finishes. No Kraken Suite, SDRplay API or other
vendor service is stopped or upgraded by these hooks.

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

Use this route for development or a system outside the package targets.

### 1. Install build dependencies

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
git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
cd blah2-VectorWarp
VW_BACKEND=all
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

### 4. Open the interface and configure your receiver

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

### Package notes

The repository installer and direct-package links above cover Ubuntu 22.04,
24.04, and 26.04; Debian 13; Fedora 44; matching DragonOS bases; and 64-bit
Raspberry Pi OS Trixie. See [DragonOS](DRAGONOS.md) for Ubuntu-base selection.
