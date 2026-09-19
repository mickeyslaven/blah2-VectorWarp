# Pi 4B integration validation — 19 September 2026

This record checks the Pi implementation after integrating upstream `main`
at `68220759d77437675a9c5814768bdca2d75ba0ad`. The tested source revision is
`80bceb847df6f822cc351638879d5edc4e465aae` on
`codex/pi-arm-improvements`. Later documentation-only commits do not change
these binaries. See the [Pi 4 guide](PI4_GUIDE.md) for installation and the
[earlier mixed-worker repair](PI_MIXED_REPAIR_20260919.md) for its design and
pre-integration measurements.

## Native regression checks

The application and benchmark were built on the 8 GB Pi 4B with Raspberry Pi
OS Lite 64-bit Bookworm, using the matching mixed worker, RSPduo adapter and
capture core. Incremental compilation used two jobs after checking more than
4 GB of available memory. Builds were stopped before timing runs.

- All **31 CTest cases** passed in 15.39 seconds, including AUTO policy,
  complete-map validation, worker failures, prepared-input ownership, shared
  reference processing, and the legacy FFTW API fixture.
- All **18 processor replay cases** passed, covering supported receiver types,
  two through eight channels, clutter processing, loop/stop behavior and
  invalid or missing input.
- All **9 SDRplay SDK build-policy tests** passed. Linux packaging and
  deployment tests also passed on the Pi before the FFTW compatibility-only
  change.

The upstream integration retains cached shared-reference work for general
multi-channel processing and separate Pi workspaces for the qualified path.
It also fixes Darwin FIR shared-memory page rounding and supports FFTW 3.3.8,
whose API lacks the newer planner-thread getter. The mixed worker records the
configured FFTW budget and checks its two actual clutter CPU slots.

Separate physical generic Vulkan checks were run at `603aebe`, before the
FFTW compatibility change: the first quick check passed six small cases,
then hit a 30-second worker startup timeout and fell back to CPU. An identical
rerun passed all ten cases. The transient startup cause was not isolated.
The GPU clutter check passed, including its expected CPU clutter fallback
when a composed map exceeded the accuracy bound. These generic checks are
separate from the qualified Pi mixed-path comparisons below.

## Recorded correctness

CPU, normal FP64 mixed input, and packed paired mixed input each passed all
40 full complex-map comparisons against the frozen CPU oracle. Detector,
tracker and IQ JSON matched the frozen output byte for byte. Both mixed arms
published 40 GPU/CPU worker results with zero CPU fallbacks. Every packed
frame used the prepared-input path.

| Input path | Frames | RMS relative map error | Peak relative map error |
| --- | ---: | ---: | ---: |
| CPU | 40 | `4.4169683e-11` | `6.0047795e-10` |
| Mixed, FP64 | 40 | `3.9344904e-6` | `4.7094878e-5` |
| Mixed, packed paired | 40 | `3.9344904e-6` | `4.7094878e-5` |
| Mixed, reversed channels | 8 | `2.9855421e-6` | `3.5168291e-5` |

The bound is `1e-4` for both error measures. The reversed-channel case used
an independent eight-frame CPU golden map and also matched downstream JSON
exactly. An unsupported forced-mixed geometry was rejected before output
files were created. Packed recorded input is a correctness fixture with
reconstruction outside its processing timer; its timing is not a live result.

## Short live comparison

Four successive 30-second checks ran in CPU / AUTO / AUTO / CPU order with
real SDRplay capture, detection and tracking enabled. Each observed 62 CPIs.
The workload was two 2 MS/s channels at 551 MHz, 500 ms CPI, 410 clutter taps,
411 delay bins and 301 Doppler rows spanning ±300 Hz. The Pi used its stock
1.8 GHz CPU with the performance governor and 500 MHz V3D GPU. The
[service environment](../contrib/systemd/pi4-rspduo-performance.conf) matches
these runs. Component timing logs were enabled; no sampling profiler ran.

CPU rows exclude the first two warm-up CPIs. AUTO rows start strictly after
the frame that completes qualification, excluding the final mixed trial.
Percentiles use linear interpolation. Times are milliseconds.

| Run | CPIs | Mean | p95 | p99 | Maximum |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU 1 | 60 | 377.562 | 396.597 | 401.577 | 402.22 |
| AUTO mixed 1 | 51 | 334.552 | 346.785 | 352.150 | 355.64 |
| AUTO mixed 2 | 51 | 347.726 | 363.985 | 366.740 | 367.05 |
| CPU 2 | 60 | 379.156 | 399.615 | 409.862 | 423.45 |

Pooled means are **378.359 ms CPU versus 341.139 ms AUTO mixed**, a
**37.220 ms / 9.84% reduction**. Both mixed means meet the 350 ms target;
individual CPIs can exceed it. This short sample supports retention of the
mixed speedup after integration, not a statistically established improvement
over the earlier 339.486 ms pooled result.

Both AUTO runs selected `vulkan+cpu` and kept it for all 51 CPIs after
qualification, with no fallback. Their CPU/mixed qualification medians were
387.24/336.12 ms and 370.61/348.70 ms. Worker startup confirmed four configured
FFTW threads and two actual clutter CPU slots. Every run stopped cleanly
with zero capture/paired-queue drops, no reported capture faults and zero
final complete-CPI backlog.

All 56 worker submissions in each AUTO run used packed paired input,
including the two accuracy shadows and three mixed timing trials. A post-run
snapshot reported `throttled=0x0` and 36.0°C; this was not continuous thermal
telemetry.

Startup remains part of the evidence: AUTO's all-observed means were
352.260 and 362.871 ms, with maxima of 835.77 and 820.59 ms. Each had two
CPIs at or above 500 ms during startup checks. The CPU runs each had one
startup CPI above 500 ms, at 523.13 and 521.98 ms. No CPI in the table's
populations reached 500 ms. A successful short run does not eliminate startup
latency or replace endurance validation.

## Evidence and reproduction

Evidence remains on the Pi at `/var/tmp/vectorwarp-pi-publish-20260919/`:

- `ctest.log`, `processor-replay.log`, `sdk-tests.log`, `packaging-tests.log`
- `results/publish-{cpu,mixed,paired}*` and `results/publish-reverse-*`
- `results/publish-live-{cpu1,auto1,auto2,cpu2}*`
- `results/publish-live-summary.json` and `binaries.sha256`

Use the recorded and live commands in the
[repair report](PI_MIXED_REPAIR_20260919.md#reproduction), the
[complete Pi YAML](../config/config-pi4-rspduo.yml), and the service environment
linked above. Live timing includes parent decoding, packed transfer, worker
processing, IPC, detection, tracking and output costs. Keep builds and other
GPU work stopped while comparing modes.

Native SHA-256 hashes:

```text
blah2
ef694503cc5f7e0a980f0ea7ec08595d8d5102a24e6fbe1a4e14345e2ca0ef82
blah2-mixed-worker
b20851754073738d197ef0f34878d5beb54830f6ecc35f2a69bd7a7d08d14c88
blah2-receiver-rspduo.so
d66067e8983c33bf0b90fc8fb5aaa50afb447891e6ded755868ee2838c7b1f88
libblah2-capture-core.so.1.0.0
7f447d90ed03d40eb6acdac45a9715db5eb8108a932af11a30b974052905114a
bench-fast
8c7540a7cad0d8c91283256187a023b948cbb4e07c1e4bac36e4ddc5e87719b5
```

## Scope

These checks validate the integrated source and matching native binaries.
They do not qualify a new 40-minute mixed soak, a Bookworm release package,
a flashable image, or Pi 5. The previous 40-minute soak selected CPU on an
older binary. The production service was not replaced during these checks.
