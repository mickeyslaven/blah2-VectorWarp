# Raspberry Pi 4 guide

Status, 19 September 2026: Raspberry Pi 4B (8 GB) is the current target. The
test host runs **Raspberry Pi OS Lite 64-bit Bookworm**, which is also the
selected distribution baseline. A
Pi 5 profile is future work and needs its own hardware, AUTO, and performance
qualification. The older Raspberry Pi OS Trixie package route is deprecated
for new Pi installations; existing systems are not automatically migrated.

No VectorWarp Pi image has been built, flashed, boot-tested, or published.
The Imager-manifest generator and its four offline tests have passed, but an
image also needs a Bookworm ARM64 package target and clean-card acceptance.
RSPduo users must obtain and install SDRplay's vendor API themselves and
accept its terms; VectorWarp does not redistribute that API. See
[Pi image packaging](PI_IMAGE_PACKAGING.md) and
[SDRplay setup](SDRPLAY_SETUP.md).

## Qualified workload and current evidence

The Pi 4 work uses two RSPduo channels at 2 MS/s, 551 MHz, 500 ms (one-million
sample) CPIs, clutter lags -10 through 399 (410 taps), map delays -10 through
400 inclusive (411 bins), Doppler
±300 Hz (301 rows), and detection and tracking enabled. CPU range FFT padding
is 4096. These values describe the tested workload, not a universal performance
promise.

The earlier production version passed a 40-minute live AUTO soak: 375.691 ms
mean after qualification, p99 398.380 ms, maximum 470.71 ms, zero dropped
samples/faults, and zero final complete-CPI backlog. AUTO selected CPU in that
run. See the [complete soak result](PI_AUTO_SOAK_20260919.md).

The repaired isolated mixed worker then passed two short live AUTO runs at
**341.508 and 337.463 ms mean after qualification**, versus adjacent CPU
controls at 383.306 and 378.377 ms. AUTO kept mixed active in both runs, with
zero drops, faults, or final complete-CPI backlog. The pooled gain is 10.86%.
The preserved experimental mixed prototype measured 334.582 ms alongside
these runs. These are 30-second comparisons, not endurance qualification for
the repaired worker. See the [repair and validation report](PI_MIXED_REPAIR_20260919.md).

## AUTO and mixed processing

The setting is `process.performance.acceleration`. The new whole-CPI mixed
candidate is considered only when it is `auto`, Linux identifies Pi 4B/BCM2711,
the device request is generic `auto`, and the CPI has the exact geometry above
(one surveillance path, no array reference, clutter enabled, 2 MS/s,
one million samples, 301×411 map, 3322-sample correlation, 4096 range FFT,
410 clutter bins, delays -10..400, Doppler -300..300 centered at zero):

```yaml
process:
  performance:
    acceleration: auto
```

Pi 5, other hardware, explicitly selected GPU devices, and any other geometry
continue through the existing generic AUTO or explicit-acceleration policy;
this gate does not force them to CPU.

For the Pi 4 candidate, the parent keeps the CPI inputs and starts an isolated
mixed child. The child owns Vulkan and the frozen mixed DSP path; it returns a
finite complete map plus the filtered tail through bounded IPC. AUTO first
compares two child maps with the CPU map for every bin (RMS and peak relative
error must each be at most `1e-4`). It then alternates three CPU and three mixed
whole-CPI trials. Mixed is selected only when its median complete-CPI time is
strictly below 95% of the CPU median (more than 5% faster). Live accuracy
comparisons wait for a nearly empty capture queue and let CPU processing drain
backlog between comparisons; they do not increase the queue or reduce the RF
workload. Capture drops,
inadequate or growing backlog, worker/IPC
failure, invalid output, a failed map comparison, or later loss of the margin
disable mixed processing and use CPU.

“CPU” is a genuine CPU path. An explicit generic GPU request is handled by the
generic accelerator; it does not override it with the Pi mixed child. Mixed
telemetry describes the child as `vulkan+cpu` when it publishes a map. The
mixed design still contains CPU work, including the dense FP64 solve and its
CPU portions; it must not be described as a fully GPU ambiguity map.

The isolated worker now sets FFTW's planner thread count before constructing
its CPU helpers and checks that two actual clutter CPU slots exist. The older
worker silently ran with only one slot; this was the main regression from the
fast prototype. Direct paired live input also stays packed as signed16 during
the worker transfer (8 MB instead of 32 MB of FP64). The child decodes into its
persistent working buffers. The parent retains independent original samples
for CPU recovery; this is not zero copy.

The repaired worker and earlier CPU-selected soak are separate qualifications.
The soak's only ≥750 ms event was an 871.52 ms startup accuracy comparison,
which the profiler captured. Every ready-state frame stayed below 500 ms,
with peak temperature 50.634°C and no throttling. Those endurance and hardware
results apply to the old CPU-selected binary. See the
[mixed-worker repair and short comparisons](PI_MIXED_REPAIR_20260919.md) for
current correctness checks, timings, binary identities, and remaining limits.

## Related records

- [Bookworm Lite OS history](PI_OS_LITE_20260918.md)
- [Pi image packaging plan](PI_IMAGE_PACKAGING.md)
- [GPU concurrency experiments](PI_GPU_CONCURRENCY_20260918.md)
- [Mixed-worker repair and short comparisons](PI_MIXED_REPAIR_20260919.md)
- [40-minute AUTO soak result](PI_AUTO_SOAK_20260919.md)
- [40-minute soak diagnostics](PI_SOAK_DIAGNOSTICS.md)
