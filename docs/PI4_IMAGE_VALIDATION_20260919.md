# Pi 4 Bookworm image preview validation — 19 September 2026

The preview supplies a complete Raspberry Pi OS Lite 64-bit Bookworm image,
VectorWarp, the web interface, private Node runtime, Mesa Vulkan drivers, and
our locally buildable RSPduo adapter source kit. The hardware target is a
Raspberry Pi 4B with 8 GB RAM. Pi 5 is future work.

The [downloadable preview](https://github.com/mickeyslaven/blah2-VectorWarp/releases/tag/v0.1.10-pi4-preview)
passed the software checks below. It is a preview because fresh-card hardware
acceptance remains outstanding.

## Inputs and package acceptance

- Application and image-builder source: `dfb16720510eaf68d5ffecc9e32a290997503897`.
  Later branch commits update documentation and verification tools.
- Official base: `2026-09-15-raspios-bookworm-arm64-lite.img.xz`, SHA-256
  `bcaefdf9c40dbed31dcaeb3b8494e498b4f1e3078c2604b0d9f5f595f8f6fd91`.
  Its hash was checked before assembly; the test Pi's installed root was not
  used as a distribution image.
- Package: `vectorwarp_0.1.10-1_debian12_arm64.deb`, 36,387,872 bytes, SHA-256
  `b0e0e7853a5b8c28a99474ade1230b84a487d4989221b22f771ffeb04c358869`.
- Native ARM64 Release build on Debian 12, two build jobs, Node 24.21.0, pinned
  vcpkg and VkFFT dependencies. Kraken, USRP and HackRF adapters are compiled;
  SDRplay support is built locally after the user installs its vendor API.
- **18/18 processor replay cases passed** with the new package's binary,
  including invalid-input rejection, 2–8 channel layouts, EOF, loop, missing
  files, mismatch rejection, and 6 MS/s USRP/HackRF playback with clutter.
- Linux packaging checks passed. Installer tests: 17 cases, one opt-in skip;
  image-builder argument tests: four passed on Linux.

## Image checks

Assembly installs the actual DEB and resolves its dependencies using the
base image's signed OS repositories. Internal checks passed for the neutral
Pi configuration, GPU groups and drivers, service policy, locked factory
accounts, absent vendor SDK libraries, and absence of benchmark recordings
and developer keys. The image retains the official Imager/first-boot hooks.

The final compressed image **passed** checks for its FAT/ext4 partition layout,
all required ARM64 executables, and complete shared-library dependencies. Its
normal web API **passed** as the packaged service user in a disposable copy with
private mount, network and PID namespaces: the HTML homepage, `/api/config`, and
`/api/system/status` all responded successfully. This used the real application,
not preview mode. Official first-boot scripts and the receiver-management socket
dependency were present.

The actual manifest passed the official Imager Draft-07 JSON schema. Opening it
in Raspberry Pi Imager **2.0.10 on macOS** displayed one device, Raspberry Pi 4
Model B, and the VectorWarp 0.1.10 Pi preview in the OS list. Storage writing and
credential customization were not performed.

The image enables web setup at boot while leaving radar stopped. Its starting
configuration is neutral 100 MHz, unset receiver/transmitter sites, 2 MS/s,
500 ms CPI, ±300 Hz, one surveillance channel, tracking and AUTO acceleration.
The included dual-RSPduo performance profile enables the packed paired queue,
bulk transport, measured FFT plans, two clutter CPU slots, one BLAS/OpenMP
thread, and the ratio-three counter guard. That guard is limited to the tested
dual-tuner mode. The Pi 4 governor unit selects the stock performance governor;
it does not overclock.

The image contains no usable factory login, SSH host keys, Wi-Fi credentials,
SDRplay API/headers or test recordings. Imager must provision the user's login,
Wi-Fi/country and SSH settings before writing. Its manifest identifies Pi 4 and
uses the official `systemd` customization format.

## Download integrity and installed versions

The archive is **674,833,836 bytes** (about 675 MB), expanding to **8,589,934,592
bytes** (8 GiB). Use a 32 GB or larger card; this image does not fit a nominal
8 GB card. GitHub's uploaded asset digests were checked against the local build.
The release includes `SHA256SUMS` and the image's full package-version provenance.
These preview checksums are separate from the signed stable-package release flow.

```text
compressed image SHA-256
52f675b821c9c6bb34276748df0efca7a90df4aca9e4e5204d87cbe32a42ed39
raw image SHA-256
a8f31bd9428665b63a2f94c774b59591f6198e503cefa9f3cd17100bae93daf4
Imager manifest SHA-256
ccac38b9b5134f3192cd32e324edced2228b5afb54191fcbffae1b269f401dad
```

The image includes Raspberry Pi kernel 6.12.109, firmware
`1:1.20260907-1~bookworm`, Mesa Vulkan `24.2.8-1~bpo12+rpt5`, FFTW `3.3.10-1`,
and OpenBLAS pthread `0.3.21+ds-4`. The provenance JSON records every installed
package and the exact DEB, base image and performance-profile hashes.

## What this does not establish

The compressed image has **not been written to a fresh card or cold-booted**.
Physical Wi-Fi association, SSH provisioning, identity generation, filesystem
expansion, first-boot recovery and live SDR/GPU behavior from that newly booted
image remain unqualified. The sole remotely accessible card was not overwritten.
There is no new image endurance or 40-minute soak result.

The earlier [integrated Pi processing validation](PI4_PUBLICATION_VALIDATION_20260919.md)
measured 378.359 ms CPU versus 341.139 ms AUTO mixed across short live RSPduo
runs with tracking. Those are prior native-host measurements, not fresh-image
performance claims. New image hardware qualification must repeat the relevant
checks before promoting the preview to a supported production image.

For first boot and updates, see the [Pi 4 guide](PI4_GUIDE.md). Preview application
updates use a matching Bookworm ARM64 DEB; no VectorWarp Bookworm APT repository
is configured yet. Normal Raspberry Pi OS updates remain available through APT.
