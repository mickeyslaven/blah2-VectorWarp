# Raspberry Pi GPU setup

> This page describes the generic Vulkan driver and stage-qualification path.
> The current Raspberry Pi target is Pi 4B (8 GB) on Raspberry Pi OS Lite
> 64-bit Bookworm, installed from source. Its geometry-gated whole-CPI mixed
> AUTO path is documented in the [Pi 4 guide](PI4_GUIDE.md); no Pi image or
> Bookworm package is published, and the mixed worker has no new 40-minute
> soak result.

## 1. Install VectorWarp

Install the matching package from the [download and APT/DNF page](https://mickeyslaven.github.io/blah2-VectorWarp/#install),
or use the [source-build guide](INSTALL.md#build-from-source). Include GPU build
dependencies only when building from source.

VectorWarp uses Vulkan on Raspberry Pi too. GPU-enabled builds include
the backend, **not a replacement Mesa driver**. A missing or faulty Pi ICD
does not impose a new Mesa minimum on AMD, Intel, NVIDIA, or CPU-only systems.

Driver versions and distribution backports differ. VectorWarp reports known
compiler-risk hints but does not reject an older driver by version alone.
Startup numerical checks decide whether the selected device and processing
settings can use GPU acceleration.

## 2. Check, install, and qualify

Run on the Pi, after installing VectorWarp:

```bash
/opt/vectorwarp/libexec/vectorwarp-gpu-setup --status
sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --install-driver
```

The second command requires an interactive administrator terminal. It offers
the native package manager's normal transaction review and confirmation for
`mesa-vulkan-drivers` and required dependencies. APT candidates must be signed
native Debian/Ubuntu packages; Fedora 44 uses only its verified release and
updates repositories, installed native keys, and package signature checks.
No third-party Mesa repository, private ICD, whole-system upgrade, key import,
unattended confirmation, driver-version bypass, or radar restart is added.
Use the same command on supported 64-bit Ubuntu, Debian/Raspberry Pi OS, or
Fedora; distro availability still determines what can actually be installed.

If cached repositories contain no candidate/update, refresh the distribution's
signed metadata locally (`sudo apt-get update`, or `sudo dnf makecache --refresh`)
and retry. If the distro still offers no usable fix/backport, **setup cannot
manufacture one**: request a supported update from that distro. The helper
reports the missing path and makes no GPU-ready claim. Do not install Fedora
libraries on Debian/Ubuntu or substitute a random downloaded driver.

The source and repository installers accept `--setup-pi-gpu` to offer this
transaction **after** application installation. Package post-install hooks
never recursively invoke a package manager. Staging (`--destdir`), preflight
and dry-run never probe hardware or alter accounts; ordinary installation
does not silently change the graphics driver.

If the check reports that the processor account cannot access the render node:

```bash
sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --enable-service-access
```

This separately confirmed action appends only the actual DRM render node's
existing `render` or `video` group to the existing `vectorwarp` account. It
preserves other groups, refuses unexpected owner groups, and never
creates groups, changes device permissions, follows a render-node symlink, or
starts/restarts services. Missing nodes remain a diagnostic—not a guessed
static service group. An already-running process needs an explicit restart
before a changed group membership applies; use `vectorwarp restart` on version
0.1.7 or newer, or **Apply & Restart** in Settings.
The access check inspects Unix owner/group/mode metadata; it does not prove
ACL or SELinux access. Actual driver opening and processing qualification are
still required.

## 3. Apply the setting

Choose Automatic or GPU and apply the desired saved settings with
**Save & Restart** in Settings (or `vectorwarp restart` on version 0.1.7 or
newer when processing is already running; use `vectorwarp start` only when it
is stopped). Settings displays the
**delay–Doppler** and **clutter** stages separately.
An enumerated GPU or installed package is only *unqualified*. Fresh telemetry
can report either stage, or both, as startup-qualified. The display clears this
status after a restart or telemetry reconnection and rechecks freshness after
diagnosis finishes. A working distro backport overrides the version warning.
The report is tied to the current telemetry connections; it does not establish
processor identity, endurance or real-time performance for other settings.

## Safety and current limit

`blah2-gpu-worker --driver-status` is a separate, read-only diagnostic. It
enumerates hardware and driver properties without creating FFT plans or
opening a radio. The API bounds it through the local helper and caches the
read-only result for one minute. It exposes no privileged install endpoint.
Normal GPU ABI 3, the 30-second startup deadline, numerical acceptance gates,
finite/error checks and bounded CPU fallback are unchanged.

Passing the startup checks does not establish that a selected workload meets
its processing deadline. Check live processing timing before increasing load.

## Pi 4 driver diagnostic

> **Historical, Fedora 44 diagnostic:** the measurements in this section are
> not the current Bookworm Lite Pi 4 workload or a claim for the newer mixed
> AUTO candidate. See the [Pi 4 guide](PI4_GUIDE.md) for current limits.

The tested Fedora 44 Pi 4's installed Mesa `26.0.3-4` timed out during VkFFT
pipeline creation and fell back to CPU. Loading signed Fedora Mesa `26.1.8-1`
for the test allowed the unchanged shader to compile within the existing
30-second startup limit. The driver was extracted privately, not installed
system-wide; no processing code or numerical qualification limits were relaxed.

The matched CPU/GPU replay used the [Pi CPU comparison's](PI4_PERFORMANCE_20260911.md)
retained recording and standard profile: **527 MHz, 2.4 MS/s, two channels,
200 ms CPI, ±800 Hz, delays −10…245 and 30.604 km maximum excess path**.

| VectorWarp mode | Mean processing time per CPI | Less time than CPU |
| --- | ---: | ---: |
| CPU | 797.228 ms | — |
| GPU | 579.252 ms | 27.3% |
| Automatic | 578.138 ms | 27.5% |

Each mode had two 20-CPI runs, excluding the first eight CPIs per run: 24
measured CPIs per mode. These are full-pipeline processing times, not file-read
or startup time. GPU clutter and delay–Doppler paths were selected on all 24
measured frames in both GPU modes; the clutter solve still used the CPU.
All modes missed every 200 ms deadline. This is a separate driver comparison,
not a live-RF test or a rerun of the three-way upstream comparison.

For a normal installation, use the `--status` and `--install-driver` commands
above. They offer the signed Mesa version available from your own distribution;
they do not install this private override or guarantee that a particular fix is
available. After updating, check status again and test your processing settings.
