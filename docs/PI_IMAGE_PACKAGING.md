# Pi 4 Bookworm image maintainer guide

This guide is for maintainers building and qualifying the VectorWarp Raspberry Pi 4 image. Users should follow the shorter [Pi 4 guide](PI4_GUIDE.md).

> **Status:** the candidate image is still assembling. It is not published,
> flash-verified, or boot-tested. Do not create download links or describe it
> as an installable product yet.

The target is a **Raspberry Pi 4B with 8 GB RAM** running Raspberry Pi OS Lite
64-bit Bookworm. Use a 32 GB or larger microSD card for qualification. Pi 5 is
future work and must not appear in this image's device or performance claims.

## Inputs and boundaries

The builder consumes only local files and runs as root on native ARM64 Linux.
It never flashes a physical disk: it expands a regular image file, attaches a
new loop device, and removes it during cleanup. Run outside a timed Pi test
with at least 17 GiB free; assembly uses an 8 GiB raw staging image, compression
uses at most two threads and 512 MiB, and output compression may be large.

Use the pristine official 2026-09-15 Raspberry Pi OS Lite 64-bit Bookworm base:

```sh
base_url=https://downloads.raspberrypi.com/raspios_oldstable_lite_arm64/images/raspios_oldstable_lite_arm64-2026-09-15/2026-09-15-raspios-bookworm-arm64-lite.img.xz
base_sha256=bcaefdf9c40dbed31dcaeb3b8494e498b4f1e3078c2604b0d9f5f595f8f6fd91
curl --fail --location --proto '=https' --tlsv1.2 --output base.img.xz "$base_url"
printf '%s  %s\n' "$base_sha256" base.img.xz | sha256sum --check --strict
xz --test base.img.xz
```

The application input is the exact Debian 12 ARM64 DEB from the same source
revision. Do not substitute Debian 13/Trixie or bypass platform detection. The
current native package was built from `dfb16720510eaf68d5ffecc9e32a290997503897`;
its 18 replay tests passed. That is package evidence only: image assembly and
clean-card acceptance are still pending.

The builder installs the DEB and dependencies through the base image's signed
OS repositories. The DEB provides VectorWarp's private Node runtime and local
RSPduo source kit. It must never include the proprietary SDRplay API, headers,
installer, or shared library.

## Build a local candidate

Verify the base digest and the DEB's package name, architecture, version, and
checksum before assembly. The builder repeats the package-name/architecture and
base-digest checks, but that does not replace input review.

```sh
source_revision=dfb16720510eaf68d5ffecc9e32a290997503897
sudo packaging/pi/build-pi4-image.sh \
  --base base.img.xz \
  --base-sha256 bcaefdf9c40dbed31dcaeb3b8494e498b4f1e3078c2604b0d9f5f595f8f6fd91 \
  --deb vectorwarp_0.1.10-1_debian12_arm64.deb \
  --output vectorwarp-pi4-0.1.10-arm64.img.xz \
  --source-revision "$source_revision" \
  --allow-network
```

`--allow-network` permits only the target image's already-configured signed OS
repositories to resolve runtime dependencies. The builder preserves the
official boot partition, firmware, NetworkManager, Imager customization, and
first-boot expansion hooks. It enables the VectorWarp API but leaves radar
stopped. The initial configuration has neutral 100 MHz and zero-valued site
placeholders, plus the qualified Pi 4 RSPduo 2 MS/s profile.

The result has a sibling `.provenance.json` containing the source revision,
base and DEB hashes/sizes, installed package versions, image digest/size,
service state, first-boot contract, and Pi profile digest. Retain it with the
candidate. The image contains no factory login, Wi-Fi profile, SSH host keys,
machine identity, recordings, shell history, test data, or SDRplay files.

## Verify software without claiming a boot

Run the structural verifier after every successful assembly:

```sh
sudo packaging/pi/verify-pi4-image.sh \
  --image vectorwarp-pi4-0.1.10-arm64.img.xz \
  --workdir /var/tmp/vectorwarp-image-check
```

The verifier mounts a decompressed copy read-only through a new loop device and
checks Bookworm/ARM64 identity, installed dependencies, service state, clean
identity, absence of SDRplay files, native ELF architecture and dynamic-library
closure. It copies the root to a disposable private mount/network/PID namespace
and starts the normal API for `/api/config` and `/api/system/status` smoke
checks.

These are software checks. They do **not** prove a Pi can boot, Imager
customization works, Wi-Fi joins, SSH is reachable, GPU acceleration operates,
an SDR is usable, or radar processing is reliable.

## Imager metadata and candidate workflow

Generate a `.rpi-imager-manifest` only from the final `.img.xz`, so its
compressed and raw hashes and sizes are measured rather than guessed. The URL
must name the exact future artifact and release tag; do not publish a receipt or
repository entry before those files are actually uploaded.

```sh
python3 packaging/pi/generate-imager-manifest.py \
  --image vectorwarp-pi4-0.1.10-arm64.img.xz \
  --url https://github.com/mickeyslaven/blah2-VectorWarp/releases/download/v0.1.10-pi4-preview/vectorwarp-pi4-0.1.10-arm64.img.xz \
  --icon-url https://raw.githubusercontent.com/mickeyslaven/blah2-VectorWarp/dfb16720510eaf68d5ffecc9e32a290997503897/html/favicon/favicon-196x196.png \
  --version 0.1.10-pi4-preview \
  --release-date 2026-09-19 \
  --output vectorwarp-pi4-0.1.10.rpi-imager-manifest
sha256sum vectorwarp-pi4-0.1.10-arm64.img.xz \
  vectorwarp-pi4-0.1.10-arm64.img.xz.provenance.json \
  vectorwarp-pi4-0.1.10.rpi-imager-manifest > SHA256SUMS
```

The generated manifest has the Pi 4-only `systemd` Imager contract. It enables
Imager's hostname, login, locale, Wi-Fi/country, and SSH customization; a bare
local `.img.xz` does not provide those choices. See Raspberry Pi's
[local-manifest documentation](https://github.com/raspberrypi/rpi-imager/blob/main/doc/local_json/README.md).

`.github/workflows/pi-image.yml` provides the repeatable manual path. It runs
on native `ubuntu-24.04-arm`, needs 32 GiB free for assembly plus isolated smoke
verification, and accepts a stable package tag plus a safe intended Pi-image
tag. It checks that the triggering source is the immutable package tag, verifies
the signed release checksums/key fingerprint/package manifest, verifies the
pinned base image, builds, verifies, generates metadata, and uploads an Actions
artifact. It does not create a GitHub Release or claim hardware verification.

## Runtime and receiver policy

The image boots the API only; radar remains stopped until the operator reviews
Settings and chooses **Save & Restart**. For RSPduo, the operator must install
and accept the vendor API, then choose **Build SDRplay support** before the first
start. Link users to [SDRplay setup](SDRPLAY_SETUP.md); do not redistribute any
vendor component.

The image includes the Pi 4 RSPduo performance drop-in and stock BCM2711
performance-governor unit. Its counter-ratio-three guard is valid only for the
qualified dual-tuner 6 MS/s ADC / 2 MS/s output mode. Before selecting another
RSPduo mode, remove or replace the drop-in. Other receiver profiles ignore
these RSPduo-specific flags.

## Preview update policy

The preview image deliberately adds no VectorWarp APT repository because the
public stable repository does not yet provide Bookworm packages. Until that
changes, update VectorWarp only with a future matching Bookworm DEB:

```sh
sudo apt install ./vectorwarp_X.Y.Z-1_debian12_arm64.deb
```

This preserves user configuration and the image's drop-in while letting
Raspberry Pi OS resolve dependencies. Maintain the OS separately with
`sudo apt update && sudo apt full-upgrade`. Do not promise
`apt upgrade vectorwarp` for the preview application or require a reflash for a
routine matching-DEB update.

## Clean-card promotion criteria

Do not publish after structural verification alone. Promote only after all of
the following are recorded against the exact image hash and source revision:

1. Open the manifest in current Raspberry Pi Imager on supported desktop OSs;
   customize hostname, credentials, locale, Wi-Fi/country, and SSH; write and
   verify the card. Include Ethernet-only and Wi-Fi credentials with spaces and
   punctuation.
2. Boot fresh cards on Pi 4B 8 GB without a display. Check network join, mDNS
   or DHCP access, configured SSH, partition expansion, fresh machine/host
   identity, API readiness, and radar remaining stopped before setup.
3. Check incorrect-Wi-Fi recovery over Ethernet, plus a second card/device to
   detect cloned identity or provisioning state.
4. Complete supported receiver setup and live processing. Verify CPU fallback,
   AUTO qualification/selection behavior, configuration persistence, reboot,
   and the intended RSPduo vendor-API rebuild path without shipping that API.
5. Record image size, boot time, memory, temperature, CPI behavior, the tested
   Imager versions, package/base/provenance hashes, and any driver or OS update
   used for the candidate.

After promotion evidence is complete, upload the image, provenance, manifest,
and checksums to the intended release tag. Then create the checked-in release
receipt and let the package repository page derive its links. Until then, the
candidate remains unpublished.
