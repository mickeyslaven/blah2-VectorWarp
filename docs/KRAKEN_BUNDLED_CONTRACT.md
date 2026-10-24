# Pinned Kraken companion and host ingestion timing

The Linux native build and the macOS/Homebrew build use the same entry point,
`script/build-kraken.py` (the historical `build-kraken-macos.py` remains the
implementation for compatibility), source pins in `config/kraken-macos-sources.json`,
and ordered reviewed patches. No installed Heimdall is discovered through PATH.
Linux embeds the pinned static Kraken RTL driver with kernel-driver detachment;
macOS resolves the companion's private driver. x86 builds use a portable x86-64
baseline rather than the build host's instruction set. Pi uses the arm64 build.

The build receipt records archive revisions/checksums, every patch hash, compiler,
CMake, flags, architecture, and output hashes. The CycloneDX inventory identifies
the pinned source components. This is reproducible input provenance, not a claim
of byte-identical binaries across different system compilers or libraries. System
libusb, Eigen, SSL, libc and compiler inputs remain distribution dependencies.
Homebrew resources are checksum-verified by Homebrew before staging. Downloaded
archives are checked before extraction. No build/test command opens a receiver.

The existing Homebrew formula prohibits distributing prebuilt Heimdall until
upstream repository-wide redistribution terms are clarified. This change retains
that restriction: native source installs and explicitly marked test packages can
include the locally built companion; published DEB/RPM packaging is blocked. A
reviewed local source-build kit for binary-package users is still required before
release acceptance. This branch must not be represented as release-complete.

## Lifecycle and rollback

The installed local service runs only for a live Kraken profile explicitly using
127.0.0.1. It resolves the executable from the current immutable release, verifies
its receipt, owns its child, waits for instance-token/settings readiness, and
reports readiness to systemd. Processor stop/restart and package quiescence stop
the owned receiver. Remote endpoints and replay profiles skip local USB startup.
An occupied receiver endpoint fails startup; it never adopts or kills a different
Heimdall. Fresh package installation keeps processing stopped. The CLI's existing
checked processor start path pulls in this service. The bundled controller uses
the package's private Node runtime in binary test packages and system Node in
source installations.

Rollback must quiesce processing, restore the complete previous release and its
matching integration units, then explicitly start the intended stack. Never swap
only Heimdall or librtlsdr. The service receipt check rejects replaced binaries.
Historical releases lacking the bundled companion retain their existing receiver
setup; this change does not silently migrate or remove a third-party service.

Stable watchdog/config-guard helpers resolve API modules through `current/api`.
They can therefore survive release switches without importing a nonexistent
`/opt/vectorwarp/api` directory.

## Wire contract

Legacy MCHQ remains supported. The bundled owner sets `HEIMDALL_TIMING_V1=1` to
select MCHT. Old clients cannot silently interpret the new extension as samples.
MCHT retains the 32-byte big-endian fixed header and little-endian per-channel
frequency/gain pairs. A 96-byte big-endian extension follows channel metadata:

| Offset | Type | Meaning |
|---|---|---|
| 0, 4 | u32 | Version 1, length 96 |
| 8, 12 | u32 | Availability flags, reference channel 0 |
| 16, 24 | u64 | Acquisition epoch, reference-channel first sample counter |
| 32 | u64 | Reference-channel USB callback sequence |
| 40, 48, 56 | u64 | Host MONOTONIC_RAW, TAI, REALTIME nanoseconds |
| 64, 72 | u64 | Clock-read span, kernel discipline maximum error nanoseconds |
| 80, 88 | u64 | Published buffer sequence, one-past sample at the USB clock anchor |

Flags are 1 for MONOTONIC_RAW, 2 for TAI with a configured positive kernel TAI
offset, and 4 for kernel clock discipline not reporting unsynchronized/error.
Unavailable TAI and kernel-error values are zero; zero is not an accuracy claim.
macOS supplies MONOTONIC_RAW and REALTIME without claiming Linux TAI discipline.
The reference callback's counters include discarded transfers. Acquisition epochs
invalidate hardware transitions and mixed sets. Unknown versions, flags, lengths,
anchor ranges, and invalid dimensions fail closed. Cropped clips advance the first
sample counter while retaining the original USB anchor sample and timestamp.

The timestamp is taken at the earliest Heimdall RTL callback, before copying,
queuing, DSP, and TCP delivery. It is **not ADC hardware UTC**. USB buffering,
driver latency, sample-clock offset and calibration/filter delay remain unknown.
Kernel maximum error and clock-read span must never be presented as the total RF
uncertainty. CLOCK_TAI availability does not establish PPS or an ADC clock lock.

VectorWarp uses the callback REALTIME anchor for the existing nominal-sample-rate
host fit and retains raw monotonic/TAI/discipline evidence in frames and sidecars.
Sample, callback, buffer, acquisition-epoch or protocol discontinuities reset the
coherent epoch. Missing PPS/TAI/discipline does not suppress coherent detections,
provisional associations or corpus capture. Event-time remains a provisional host
estimate and decision-grade/hardware-UTC claims remain false. Calibrated timing
uncertainty and rate-expanded association gates are downstream policy, not proof
of hardware synchronization.

## Validation boundary

Deterministic tests cover legacy/new fragmentation, no-PPS operation, sequence
gaps/reordering, retune epochs, invalid protocol versions, stable installed helper
paths, replacement-binary rejection, and bounded automatic-gain parsing. The real
patched Heimdall is tested with a fake RTL ABI; production binaries are compiled
but never executed. Native macOS and Fedora CPU builds provide build evidence.
CI includes Ubuntu x86/arm64 simulation and distro-native package checks. This is
software validation; physical PPS, USB scheduling/jitter, RF coherence and live
receiver qualification remain a separate hardware exercise.
