# Raspberry Pi 4 guide

The supported Pi profile is a **Raspberry Pi 4B with 8 GB RAM** running
**Raspberry Pi OS Lite 64-bit Bookworm**. Use a 32 GB microSD card initially.
The Bookworm Lite preview is available through its
[Imager manifest](https://github.com/mickeyslaven/blah2-VectorWarp/releases/download/v0.1.10-pi4-preview/vectorwarp-pi4-0.1.10.rpi-imager-manifest)
and [release page](https://github.com/mickeyslaven/blah2-VectorWarp/releases/tag/v0.1.10-pi4-preview).
Its internal software checks passed, but fresh-card boot and Wi-Fi qualification
are pending. See [validation limits](PI4_IMAGE_VALIDATION_20260919.md). Pi 5 is
future work.

## Flash and first boot the preview

1. Install Raspberry Pi Imager, download and open the versioned
   [`vectorwarp-pi4-0.1.10.rpi-imager-manifest`](https://github.com/mickeyslaven/blah2-VectorWarp/releases/download/v0.1.10-pi4-preview/vectorwarp-pi4-0.1.10.rpi-imager-manifest),
   and choose the Pi 4 Bookworm Lite image. The manifest is required for Imager
   customization; selecting only the bare `.img.xz` does not provide the
   headless Wi-Fi, login, and SSH flow.
2. In Imager customization, set hostname, locale/time zone, a username with a
   password or SSH key, and enable SSH. For Wi-Fi, also set the network name,
   password, and country. Ethernet can omit Wi-Fi. The image contains no factory
   login or preconfigured network.
3. Write the card, boot the Pi, and connect over SSH or open
   `http://<hostname>.local:3000/` from a device on the same network. Use the
   DHCP address if mDNS is unavailable. For an RSPduo, obtain, install, and
   accept [SDRplay Hardware API 3.15 for Linux ARM64](SDRPLAY_SETUP.md) yourself,
   then choose
   **Build SDRplay support** in Settings and wait for the local build.
   VectorWarp does not distribute SDRplay software.
4. Choose the receiver in Settings and review the neutral 100 MHz frequency and
   zero-valued site placeholders. For an RSPduo, complete the vendor API and
   **Build SDRplay support** step first. Then select **Save & Restart** to start
   processing.

The image profile prepares the tested Pi 4B 8 GB RSPduo geometry: 2 MS/s,
500 ms CPI, ±300 Hz (301×411), tracking, and Automatic acceleration. It starts
with radar stopped. Its counter-ratio-three performance drop-in is valid only
for that dual-tuner 2 MS/s mode; remove or replace it before selecting another
RSPduo mode. Other receiver profiles ignore those RSPduo-specific flags.

This preview has not completed the flow on a clean card. Review
[validation limits](PI4_IMAGE_VALIDATION_20260919.md) before flashing; it does
not yet establish fresh-card boot, Wi-Fi, SSH, receiver, or radar reliability.

## Updating a preview image

The preview image does not preconfigure the stable VectorWarp APT repository.
For application updates, use the [installation page](https://mickeyslaven.github.io/blah2-VectorWarp/#install)
to configure the signed Bookworm repository, then update with APT. You can instead
obtain a matching stable Bookworm ARM64 DEB and install that exact file:

```sh
sudo apt install ./vectorwarp_X.Y.Z-1_debian12_arm64.deb
```

Use a release-supplied `vectorwarp_<version>-1_debian12_arm64.deb`; do not use
a Debian 13/Trixie package. This command updates VectorWarp while resolving its
dependencies from Raspberry Pi OS and preserves the image's Pi 4 RSPduo
performance drop-in. Continue normal OS maintenance separately:

```sh
sudo apt update && sudo apt full-upgrade
```

After configuring the stable Bookworm repository, update with:

```sh
sudo apt update && sudo apt install vectorwarp
```

## Advanced: source installation on Bookworm

1. In Raspberry Pi Imager, select **Raspberry Pi OS Lite (64-bit) Bookworm**.
   Before writing the card, set the hostname, locale/time zone, Wi-Fi name,
   password and country, a username with password or SSH key, and enable SSH.
   Write the card, boot the Pi, then connect by SSH. The downloadable preview
   image is the preferred Pi 4 route; this source path is an advanced alternative.
2. Install the Linux build prerequisites in the
   [source-install guide](INSTALL.md#1-install-build-dependencies), including
   Node.js 22 or later at `/usr/bin/node`. For the Vulkan candidate, also install
   the guide's Vulkan and glslang development packages.
3. Clone, preflight, build, and install. `--jobs 1` is accepted by
   `build-native.sh` and reduces peak build memory on the Pi; remove it only if
   the Pi has sufficient memory and cooling for more parallel work.

   ```sh
   git clone https://github.com/mickeyslaven/blah2-VectorWarp.git
   cd blah2-VectorWarp
   script/build-native.sh --preflight --backend all --gpu auto --jobs 1
   script/build-native.sh --backend all --gpu auto --jobs 1
   sudo script/install-native.sh --preflight
   sudo script/install-native.sh
   ```

   `--backend all` builds Kraken, USRP, and HackRF support plus the local
   RSPduo source kit. `--gpu auto` is the build default and includes Vulkan
   when its dependencies are usable. The installer preserves existing
   `/etc/vectorwarp/config.yml` and does not start VectorWarp.
4. Check the GPU driver with
   `/opt/vectorwarp/libexec/vectorwarp-gpu-setup --status`. If it reports a
   missing driver, run
   `sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --install-driver` and review
   the distribution's Mesa transaction. If service-account access is missing,
   run the helper's `--enable-service-access` action with `sudo`. See
   [Pi GPU setup](PI_GPU_SETUP.md) for details.
5. Run `vectorwarp` over SSH. It starts only the web API when needed and prints
   its address. From another device on the same network, open
   `http://<pi-address>:3000/`; do not use `localhost` from that other device.
   Keep this unauthenticated UI on a trusted LAN/VPN or behind an authenticated
   gateway.
6. For an RSPduo, install and accept SDRplay's vendor Hardware API yourself.
   VectorWarp does not distribute it. In Settings, select **RSPduo**, use
   **Build SDRplay support**, wait for the local adapter build to complete, then
   configure the receiver and choose **Save & Restart**. See
   [SDRplay setup](SDRPLAY_SETUP.md) for the vendor/API service details.

## Tested RSPduo configuration

The complete [Pi 4B example YAML](../config/config-pi4-rspduo.yml) contains this
workload, one surveillance worker, four FFT threads, and tracking enabled.
Use it as a reference when editing Settings; retain your receiver serial,
network, recording and site values. The example's 551 MHz transmitter and
neutral site coordinates must be adapted to your actual installation.

Use two RSPduo channels at **2 MS/s**, **551 MHz**, and **500 ms CPI** (one
million samples). Enable clutter, detection, and tracking; use one surveillance
path and no array reference. Set clutter lags **-10…399** (410 taps; the YAML
upper bound is exclusive, so use `delayMax: 400`), delay map **-10…400** (411 bins),
and Doppler map **±300 Hz** (301 rows). The qualified Pi path chooses CPU range
FFT padding **4096** internally. These settings describe the tested workload; they are not a
general real-time promise.

Set **Settings → Processing → Acceleration** to **Automatic**. To be eligible
for the Pi-specific mixed path, keep the device request at generic `auto` and
keep exactly the Pi 4B/BCM2711, one-surveillance, no-array-reference,
clutter-enabled 2 MS/s, one-million-sample, 301×411, 410-tap geometry above.
An explicit GPU device or any other geometry follows the generic CPU/Vulkan
policy.

### Apply the tested service environment

The image already includes the Pi 4 RSPduo performance drop-in and its stock
BCM2711 performance-governor unit. A matching-DEB update preserves that image
configuration. The following opt-in commands apply only to a source install or
an existing operating-system installation being configured for this exact
dual-RSPduo profile; run them before choosing **Save & Restart**:

```sh
sudo install -d /etc/systemd/system/vectorwarp-processor.service.d
sudo install -m 0644 contrib/systemd/pi4-rspduo-performance.conf \
  /etc/systemd/system/vectorwarp-processor.service.d/pi4-rspduo-performance.conf
sudo systemctl daemon-reload
```

The drop-in enables the bounded paired CPI queue and bulk USB transport, uses
measured FFT plans and two clutter CPU slots, and limits BLAS/OpenMP to one
thread. Its sample-counter ratio of three is accepted only for the tested
dual-tuner 6 MHz ADC / 2 MS/s output mode; another receiver mode requires a
separately qualified profile. These settings apply on the next processor
start. An already-running processor needs `vectorwarp restart` after saving
the configuration. Remove this drop-in and reload systemd to return to the
normal service environment.

The short comparisons also used a stock 1.8 GHz Pi 4 CPU with the performance
governor and cooling that avoided throttling. The drop-in does not change the
OS governor or clocks. Faster startup is not promised: measured FFT planning
and AUTO's accuracy comparisons happen before steady operation.

## Check that it is running

After **Save & Restart**, confirm fresh receiver and processor status in
Settings, then use:

```sh
vectorwarp status
vectorwarp logs
```

For the exact eligible workload, successful qualification may report the mixed
backend as `vulkan+cpu`. CPU is also a valid AUTO outcome when the timing or
map checks do not accept mixed processing. Confirm that the map updates and
detection/tracking are active before treating the setup as successful. For a
driver diagnostic, run `/opt/vectorwarp/libexec/vectorwarp-gpu-setup --status`.

## Evidence and limits

The integrated source passed two short live AUTO runs at **334.552
and 347.726 ms** mean after qualification, versus adjacent CPU controls at
**377.562 and 379.156 ms**. AUTO kept mixed active with zero drops, faults, or
final complete-CPI backlog: a **9.84% pooled** improvement. These were
30-second comparisons, not endurance qualification. No new 40-minute mixed
soak has passed. An earlier 40-minute AUTO soak passed with CPU selected;
that result applies only to the older CPU-selected binary. See the
[integration validation](PI4_PUBLICATION_VALIDATION_20260919.md),
[earlier repair report](PI_MIXED_REPAIR_20260919.md) and
[earlier soak result](PI_AUTO_SOAK_20260919.md).

## AUTO and mixed processing

For the eligible Pi 4 configuration, the parent starts an isolated mixed child
that owns Vulkan and the mixed DSP path. It first compares two complete child
maps with CPU maps (RMS and peak relative error must each be at most `1e-4`),
then alternates three CPU and three mixed whole CPIs. Mixed is selected only
when its median CPI is more than 5% faster than CPU.

The paired live input crosses the worker boundary as packed signed 16-bit data
(8 MB rather than 32 MB of FP64); the child decodes it into persistent buffers.
The parent keeps the original samples and switches to CPU for a worker crash or
hang, IPC failure, invalid/non-finite map, failed comparison, capture drop,
unacceptable backlog, or loss of the timing margin. `vulkan+cpu` includes the
worker's CPU work, including the FP64 coefficient solve, and is not a fully GPU map.

## Related records

- [Integrated-source regression and live checks](PI4_PUBLICATION_VALIDATION_20260919.md)
- [Bookworm Lite OS history](PI_OS_LITE_20260918.md)
- [Pi image packaging plan](PI_IMAGE_PACKAGING.md)
- [GPU concurrency experiments](PI_GPU_CONCURRENCY_20260918.md)
- [Mixed-worker repair and short comparisons](PI_MIXED_REPAIR_20260919.md)
- [40-minute AUTO soak result](PI_AUTO_SOAK_20260919.md)
- [40-minute soak diagnostics](PI_SOAK_DIAGNOSTICS.md)
