# Headless Raspberry Pi image

> **Maintained status:** The target is a Raspberry Pi 4B Bookworm Lite 64-bit
> image; Pi 5 is future work. A candidate is being built, but no flashable image
> has been boot-tested or published. See the concise [Pi 4 guide](PI4_GUIDE.md) for current
> installation and AUTO boundaries.

Status: the Pi 4 Bookworm candidate is being assembled from a pristine
2026-09-15 Raspberry Pi OS Lite image and the Bookworm ARM64 DEB. It is not
boot-tested or published. The mixed CPU/GPU AUTO integration is proceeding
separately.

## Product decision

Confirmed by the maintainer: **Bookworm is the Pi 4 image baseline**, and the
previous Pi installation route can be deprecated. There is no requirement to
preserve that route as the basis of the new image.

After the acceptance work below completes, a release could offer **VectorWarp
for Raspberry Pi 4** as a downloadable `.img.xz` with a companion
`.rpi-imager-manifest`. Neither artifact exists or is downloadable today. The
planned image would use **Raspberry Pi OS Lite 64-bit Bookworm**, matching the
OS family used for current Pi 4 performance work, and run the native processor,
API, and browser UI without a desktop or Docker runtime.

**Raspberry Pi 5 is future work.** Keep it out of the initial image's supported
device list and release claims. It needs its own physical boot, receiver, driver,
AUTO-selection and performance qualification; Pi 4 GPU work allocation is not a
Pi 5 performance profile. Share application packages and image stages where
possible, then add a separately qualified Pi 5 image/profile.

The bench machine is a Pi 4B with 8 GB RAM. The first physical acceptance target
is that configuration; qualify other Pi 4 memory sizes before advertising them.
Recommend a 32 GB microSD card initially, but determine and publish the actual
minimum card size from the finished image and first-boot expansion tests.

## Intended user flow after publication

These are release instructions to validate before publication, not steps for a
currently available VectorWarp image.

1. Install the current Raspberry Pi Imager on Windows, macOS or Linux.
2. Download and open `vectorwarp-pi4.rpi-imager-manifest`. The manifest points
   Imager to the versioned compressed image; users do not need Python or a shell.
3. Select the microSD card. In customization, set hostname, username/password
   (or SSH public key), locale/time zone, Wi-Fi name/password and Wi-Fi country.
   For headless Wi-Fi installation, complete these fields before writing.
   Enable SSH for remote administration; a wired Ethernet setup may omit Wi-Fi.
4. Write and verify the card, insert it into the Pi, connect the receiver and
   power it on. Network provisioning and filesystem expansion run on first boot.
5. Open `http://<hostname>.local:3000/` from another device on the same network
   (for example `http://vectorwarp.local:3000/`). Use the router's DHCP address
   if mDNS is unavailable. Settings handles receiver/site setup; capture starts
   only after the user saves a valid configuration and requests it.

Imager's manifest association has a GUI fallback: App Options → Content
Repository → Edit → Use custom file, then select the manifest and restart the
Imager session. Record the exact tested Imager versions in each image release.
Use the current supported Imager rather than promising an untested minimum.

This manifest is necessary for a reliable headless flow: the current Imager
documentation says an arbitrary local image selected with **Use custom** lacks
customization metadata and therefore does not offer the setup options. A
manifest identifies the appropriate mechanism. See the official
[local-manifest documentation](https://github.com/raspberrypi/rpi-imager/blob/main/doc/local_json/README.md).
Do not instruct users to flash a bare file and assume Wi-Fi will be configured.
Stock Imager can still let users skip customization; metadata enables the
wizard but cannot force a Wi-Fi password. Our installation instructions and
acceptance tests must explicitly cover that step.

An online VectorWarp Imager repository can use the same metadata. A listing in
the official Imager catalog is a later distribution improvement requiring their
review, not a prerequisite for the first release. Download + manifest avoids a
custom flasher. Raspberry Pi documents custom repositories and image listing
requirements in its [vendor guide](https://www.raspberrypi.com/news/how-to-add-your-own-images-to-imager/).

## Image construction

The builder customizes the pristine official **2026-09-15 Raspberry Pi OS
Lite 64-bit Bookworm** image. It verifies the pinned compressed SHA-256
`bcaefdf9c40dbed31dcaeb3b8494e498b4f1e3078c2604b0d9f5f595f8f6fd91`, expands a
new regular image file to 8 GiB, and installs the exact Bookworm ARM64 DEB into
its root filesystem. It preserves the official FAT boot partition, firmware,
networking, Imager customization and first-boot expansion hooks. It does not
clone the benchmark Pi or its remote migration partition layout.

Run on native ARM64 Linux with at least 17 GiB free, outside timed Pi tests.
The assembler accepts image files and creates its own loop device; it never
flashes a physical disk. Signed OS repositories supply runtime dependencies.
The private Node runtime and local RSPduo source kit come from the package.

```sh
sudo packaging/pi/build-pi4-image.sh \
  --base 2026-09-15-raspios-bookworm-arm64-lite.img.xz \
  --base-sha256 bcaefdf9c40dbed31dcaeb3b8494e498b4f1e3078c2604b0d9f5f595f8f6fd91 \
  --deb vectorwarp_0.1.10-1_debian12_arm64.deb \
  --output vectorwarp-pi4-0.1.10-arm64.img.xz \
  --source-revision COMMIT_SHA --allow-network
sudo packaging/pi/verify-pi4-image.sh \
  --image vectorwarp-pi4-0.1.10-arm64.img.xz --workdir /var/tmp/vectorwarp-image-check
```

Replace `COMMIT_SHA` with the exact source commit used for the package.
The output includes a provenance JSON with hashes, installed package versions,
and the enabled image profile. Compression is limited to two threads and
512 MiB. Verification checks the image's native runtime and starts the normal
API in an isolated network/PID namespace on a disposable copy. It does not
claim physical boot, radio, GPU or Wi-Fi acceptance.

The image stage must:

- Install a verified, exact-version **Bookworm ARM64 VectorWarp DEB** with native runtime dependencies, the packaged private Node
  runtime, Mesa/V3D/Vulkan support and the AUTO worker from that same release.
- Include NetworkManager, wireless firmware/regulatory data, Avahi and the
  standard Pi user/SSH provisioning hooks. Preserve Imager choices rather than
  overwriting hostname, country or credentials in our first-boot service.
- Enable the VectorWarp API at boot, with processing stopped pending user setup.
  The image builder must explicitly enable the API offline: package `postinst`
  skips live service activation when no running systemd exists in the chroot.
- Ship neutral site/receiver configuration and `acceleration: auto` once the
  integrated AUTO path passes acceptance. The candidate profile uses neutral
  100 MHz and zero-valued site placeholders, 2 MS/s, 500 ms CPI, ±300 Hz
  (301×411), tracking, and the Pi RSPduo performance drop-in; radar stays
  stopped until the user reviews settings and chooses Save & Restart. The
  benchmark's 551 MHz frequency, site data and temporary ports are not factory defaults.
- Keep the stock supported clock and record kernel, firmware, Mesa, FFTW, BLAS,
  Node and application versions. Record any performance governor/service tuning
  in the image profile; test it with normal capture and thermal conditions.
- Finalize with no developer Wi-Fi profiles, SSH keys, credentials, recordings,
  shell history, machine identity or test logs. Generate per-device SSH host
  keys/identity on boot using the OS mechanisms. No shared login password.

For this Bookworm image declare **`init_format: "systemd"`**, preserving the
Pi OS `firstrun.sh` mechanism. `cloudinit` / `cloudinit-rpi` are different image
contracts, not interchangeable labels; a later Trixie image must validate its
own cloud-init configuration. Use the exact schema spellings documented by
[Raspberry Pi Imager](https://github.com/raspberrypi/rpi-imager/blob/main/doc/json-schema/os-list-schema.json).

Pinning the base image alone does not make APT input reproducible. Retain a
package version manifest and exact application artifacts; use dated package
snapshots or retained package inputs where available. Refresh OS/security
packages in reviewed image revisions and repeat hardware acceptance after driver
changes.

## Existing packaging to reuse, and the OS gap

The existing package provides the native program, API/UI, service accounts,
systemd units, receiver setup helpers, and local RSPduo build kit. Reuse these
rather than installing a copied developer directory.

The previous Raspberry Pi OS Trixie package installation route is **deprecated
for new Pi deployments**. Keep existing artifacts available during the transition;
do not offer the replacement as released until a tested image exists. Existing
Pi installations do not receive an automatic OS downgrade. The replacement is
a fresh Bookworm image, with an explicit user-config export/import path.

The Bookworm ARM64 DEB has its own Debian 12 target and must be used for this
image. Do not substitute a Debian 13/Trixie package or bypass platform detection.

The preview image deliberately adds no VectorWarp APT repository: the public
stable repository has no Bookworm package yet. Until a stable Bookworm
repository exists, update the application with a future matching DEB supplied
for Bookworm ARM64:

```sh
sudo apt install ./vectorwarp_X.Y.Z-1_debian12_arm64.deb
```

That package update preserves user configuration and uses Raspberry Pi OS APT
only to resolve dependencies. Keep OS maintenance separate with normal
`sudo apt update && sudo apt full-upgrade`. Do not promise
`apt upgrade vectorwarp` for the preview application, and do not reflash the
card for a routine matching-DEB update. Major base-OS migration and
transactional OS rollback are separate future work; keep a documented
config-export/reflash recovery path for the initial image.

## SDRplay first-run dependency

Public VectorWarp packages intentionally include the RSPduo adapter source kit,
not SDRplay's proprietary API, headers or installer. Embedding those files in an
OS image is not authorized merely because the test Pi has a licensed local
installation. Redistribution permission has not been established in this work.

Until it is established, the image boots into usable browser setup, but RSPduo
capture requires the user to obtain/install the compatible vendor API and
accept its terms, followed by **Build SDRplay support** in Settings. See
[SDRplay setup](SDRPLAY_SETUP.md) and the
[vendor's API page](https://www.sdrplay.com/hardware-api/).
Current browser setup builds the adapter; it does not install the vendor API.
The documented API installation can be done over SSH and remains headless.
The image's RSPduo performance profile applies only to the qualified dual-tuner
2 MS/s mode; its ratio-three counter guard rejects other RSPduo modes. Other
receivers ignore the profile flags.

For a completely browser-only RSPduo setup, separately resolve redistribution
with SDRplay or design a user-authorized vendor installation workflow. Do not
promise that a generic flashed image is already ready to capture with RSPduo.
No vendor contact or software redistribution has been performed for this plan.

The current web interface has no login. Keep the initial image on a trusted LAN
or user-managed authenticated gateway, matching existing deployment guidance.
Imager's SSH password does not authenticate the browser UI. A product requiring
authenticated LAN/browser setup needs an explicit authentication feature; do
not confuse same-origin request checks with a login.

## Build and release outputs

Add an image job separate from cross-distro application packaging. It consumes
the already-verified Bookworm release package and produces:

- `vectorwarp-pi4-<version>-arm64.img.xz`.
- `vectorwarp-pi4.rpi-imager-manifest` with an immutable HTTPS image URL,
  `devices: ["pi4"]`, customization type, compressed and raw SHA-256 hashes,
  exact compressed/raw sizes and release date.
- Signed checksums, package/OS version manifest, source/build provenance,
  third-party notices, installation instructions and physical test report.

`packaging/pi/generate-imager-manifest.py` provides the metadata step. It reads
the compressed artifact and hashes both compressed and decompressed bytes;
it does not build, flash, boot-test or publish an image. Do not create a release
entry with guessed hashes or download URLs. Follow existing release protection
and signing rules; a local candidate manifest is not a published product.

## Acceptance before offering the image

1. Build from clean inputs. Verify packages, both image hashes, first-boot
   hooks and absence of device-specific secrets; validate Imager metadata.
2. Exercise the actual Imager GUI on Windows, macOS and Linux: open manifest,
   configure Wi-Fi/country/hostname/login/SSH, write and verify. Include Wi-Fi
   passwords with spaces and punctuation, Ethernet-only and omitted Wi-Fi.
3. Boot a fresh disposable card on a Pi 4B without monitor/keyboard. Verify
   network join, mDNS/IP access, configured SSH, partition expansion, fresh
   machine/host keys, API readiness and radar remaining stopped until setup.
   Repeat on a second card/device to catch cloned identity and hostname issues.
4. Test wrong Wi-Fi credentials and recovery via Ethernet. First boot must not
   wait forever for an unrelated interface. No hotspot or cloud account is
   required for the initial release.
5. Complete supported receiver setup. Check driver/worker availability, genuine
   CPU-only behavior, AUTO qualification/selection/fallback and live processing
   on the actual image. Revalidate full recorded maps/detection/tracking.
6. Validate signed application update, local RSPduo adapter rebuild behavior,
   config persistence and reboot. Record image size, boot time, steady memory,
   temperature and actual CPI measurements from this image.
7. After short live checks and outstanding capture reliability work pass, run
   the user-requested 40-minute soak. A separate supervised soak with spike
   diagnostics is currently authorized; it is not an image acceptance result
   until its owner records a successful outcome.

A chroot/QEMU check cannot certify Pi Wi-Fi, SDR USB transport, GPU behavior or
the flash-to-first-boot experience. The current Pi's remote migration and live
benchmark do not replace clean-card acceptance. Do not overwrite the sole remote
benchmark card to test this image; use a disposable target or a separately
verified reversible boot arrangement.

## Implemented so far

The Pi 4 manifest generator and four offline tests are present. Tests cover
compressed/raw hashes and sizes, Pi 4 customization metadata, malformed XZ,
atomic preservation of existing output, and unsafe URL forms. All four passed
with `jsonschema` validation against the official Imager schema retrieved on
19 September 2026 (SHA-256
`c3e323aa297e9ef3f386f522abb4497db5eb9a1ca5f7f3c7a3b1f9a75862ac85`).
This validates metadata generation, not candidate-image construction or headless
boot. The Bookworm package target, image assembler and isolated software verifier are
present. Native image assembly and acceptance results must identify the exact
artifact; physical clean-card acceptance remains separate.
