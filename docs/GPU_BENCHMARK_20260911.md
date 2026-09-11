# GPU comparison — 11 September 2026

## Result

This is the current, like-for-like replay comparison after the combined CPU,
GPU data-transfer and JSON-output improvements. Each matched pair uses the same
samples from one 20.002-second recording (SHA-256
`1e8d50a5fe62410ead9094d95a57af1d03414e87ac00b8861b75444c7aab12aa`), at
527 MHz and 2.4 MS/s, with delays −10…245 (256 bins, maximum excess path
30.604 km) and clutter −10…200.

Against actual [`30hours/blah2` `c821bee3`](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de),
the 200 ms, ±2400 Hz pair workloads take **25.1–25.9% less processing time in
VectorWarp CPU mode**, or **63.8–71.3% less in GPU mode** across these hosts.
Those are processing-efficiency gains, not evidence of improved detection
accuracy. Accuracy remains subject to the same acceptance gates.

All table figures are ms/CPI (lower is better). P95 is pooled across the 24
steady CPIs for that row; misses are steady CPI deadlines.

| Host and two-channel workload | CPI | Regular blah2 mean / p95 / misses | VectorWarp CPU mean / p95 / misses | VectorWarp GPU mean / p95 / misses |
| --- | ---: | --- | --- | --- |
| Strix, ±800 Hz | 200 ms | 79.512 / 81.171 / 0/24 | 65.495 / 72.456 / 0/24 | **31.089 / 33.969 / 0/24** |
| Strix, ±2400 Hz | 200 ms | 136.414 / 140.113 / 0/24 | 101.099 / 111.218 / 0/24 | **44.669 / 51.835 / 0/24** |
| Strix, ±800 Hz | 100 ms | 43.141 / 44.437 / 0/24 | 34.137 / 35.664 / 0/24 | **18.281 / 21.477 / 0/24** |
| RTX 4050 Laptop, ±800 Hz | 200 ms | 230.457 / 289.496 / 22/24 | 180.855 / 228.973 / 6/24 | **63.638 / 80.248 / 0/24** |
| RTX 4050 Laptop, ±2400 Hz | 200 ms | 342.597 / 403.223 / 24/24 | 256.094 / 292.303 / 24/24 | **124.097 / 143.277 / 0/24** |
| Pavilion Intel HD 630, ±800 Hz | 200 ms | 205.655 / 212.570 / 24/24 | 167.782 / 170.205 / 0/24 | **64.903 / 68.915 / 0/24** |
| Pavilion AMD Polaris 12, ±800 Hz | 200 ms | 205.655 / 212.570 / 24/24 | 167.782 / 170.205 / 0/24 | **62.212 / 65.592 / 0/24** |
| Pavilion Intel HD 630, ±2400 Hz | 200 ms | 306.828 / 312.945 / 24/24 | 229.883 / 242.064 / 24/24 | **93.106 / 99.640 / 0/24** |
| Pavilion AMD Polaris 12, ±2400 Hz | 200 ms | 306.828 / 312.945 / 24/24 | 229.883 / 242.064 / 24/24 | **88.079 / 91.125 / 0/24** |

## Strix capacity at the same full delay range

These are VectorWarp-only workloads: upstream was not timed where its geometry
is unsafe or it has no equivalent five-channel array mode. They retain the
same 527 MHz / 2.4 MS/s / −10…245 / 256-bin / 30.604 km settings.

| Workload | CPI | VectorWarp CPU mean / p95 / misses | VectorWarp GPU mean / p95 / misses |
| --- | ---: | --- | --- |
| Two-channel pair, ±4800 Hz | 200 ms | 173.773 / 179.909 / 0/24 | **67.004 / 69.787 / 0/24** |
| Five-channel array, ±800 Hz | 200 ms | 146.238 / 153.228 / 0/24 | **75.196 / 77.619 / 0/24** |
| Two-channel pair, ±4800 Hz | 1 s | 886.016 / 900.195 / 0/24 | **325.473 / 330.635 / 0/24** |
| Five-channel array, ±2400 Hz | 1 s | 1118.401 / 1140.778 / 24/24 | **622.844 / 627.700 / 0/24** |

Both CPU and GPU now meet all 24 steady deadlines for the two-channel ±4800 Hz
cases. The one-second five-channel CPU workload still misses all 24 deadlines;
its GPU counterpart misses none.

## Older GPUs: same-range capacity

The RTX 4050 Laptop campaign used four physical CPU cores; the Pavilion used
its Intel HD 630 and AMD Polaris 12 Vulkan devices. These VectorWarp-only
capacity measurements are not upstream speedups.

| Host and workload | CPI | VectorWarp CPU mean / p95 / misses | VectorWarp GPU mean / p95 / misses |
| --- | ---: | --- | --- |
| RTX 4050 Laptop pair, ±4800 Hz | 200 ms | 401.058 / 463.181 / 24/24 | **189.116 / 200.666 / 2/24** |
| RTX 4050 Laptop five-channel array, ±800 Hz | 200 ms | 480.556 / 513.677 / 24/24 | **157.266 / 251.504 / 6/24** |
| Pavilion Intel HD 630 pair, ±4800 Hz | 200 ms | 355.703 / 360.299 / 24/24 | **134.149 / 139.840 / 0/24** |
| Pavilion AMD Polaris 12 pair, ±4800 Hz | 200 ms | 355.703 / 360.299 / 24/24 | **133.241 / 139.438 / 0/24** |
| Pavilion Intel HD 630 five-channel array, ±800 Hz | 200 ms | 488.678 / 499.124 / 24/24 | **191.237 / 203.701 / 5/24** |
| Pavilion AMD Polaris 12 five-channel array, ±800 Hz | 200 ms | 488.678 / 499.124 / 24/24 | **169.178 / 175.795 / 0/24** |

Every current Automatic GPU row used **GPU delay–Doppler and GPU clutter on
all 24 steady CPIs**, with zero CPU accuracy oracles. Sustained AUTO cost
selection remains enabled. Some workloads still miss deadlines: RTX ±4800 Hz
misses 2/24, RTX five-channel 6/24, and Intel five-channel 5/24. A mean below the
CPI is not a guarantee that every frame meets its deadline.

## What the combined efficiency changes added

This separate before/after comparison reruns VectorWarp `2a9bfdf` and the
combined change on the same hosts and recorded samples, within this campaign.
Both versions already use startup-only accuracy qualification; these gains
are not from removing recurring CPU checks again. All rows below use two
channels, a 200 ms CPI and the same full delay range.

| Host and workload | Mode | Prior VectorWarp mean / p95 / misses | Current VectorWarp mean / p95 / misses | Less processing time |
| --- | --- | --- | --- | ---: |
| Strix, ±800 Hz | CPU | 77.810 / 80.840 / 0/24 | **65.495 / 72.456 / 0/24** | 15.8% |
| Strix, ±800 Hz | GPU | 39.004 / 42.461 / 0/24 | **31.089 / 33.969 / 0/24** | 20.3% |
| Strix, ±2400 Hz | CPU | 132.339 / 138.577 / 0/24 | **101.099 / 111.218 / 0/24** | 23.6% |
| Strix, ±2400 Hz | GPU | 69.650 / 71.779 / 0/24 | **44.669 / 51.835 / 0/24** | 35.9% |
| RTX 4050 Laptop, ±800 Hz | CPU | 215.256 / 260.885 / 21/24 | **180.855 / 228.973 / 6/24** | 16.0% |
| RTX 4050 Laptop, ±800 Hz | GPU | 117.746 / 142.331 / 0/24 | **63.638 / 80.248 / 0/24** | 46.0% |
| RTX 4050 Laptop, ±2400 Hz | CPU | 347.337 / 384.088 / 24/24 | **256.094 / 292.303 / 24/24** | 26.3% |
| RTX 4050 Laptop, ±2400 Hz | GPU | 211.224 / 227.961 / 18/24 | **124.097 / 143.277 / 0/24** | 41.2% |
| Pavilion, ±800 Hz | CPU | 205.164 / 208.740 / 24/24 | **167.782 / 170.205 / 0/24** | 18.2% |
| Pavilion Intel, ±800 Hz | GPU | 99.866 / 108.522 / 0/24 | **64.903 / 68.915 / 0/24** | 35.0% |
| Pavilion AMD, ±800 Hz | GPU | 94.927 / 98.870 / 0/24 | **62.212 / 65.592 / 0/24** | 34.5% |
| Pavilion, ±2400 Hz | CPU | 302.468 / 303.531 / 24/24 | **229.883 / 242.064 / 24/24** | 24.0% |
| Pavilion Intel, ±2400 Hz | GPU | 165.063 / 168.974 / 0/24 | **93.106 / 99.640 / 0/24** | 43.6% |
| Pavilion AMD, ±2400 Hz | GPU | 157.993 / 161.039 / 0/24 | **88.079 / 91.125 / 0/24** | 44.3% |

The changes reduce detection allocations and repeated calculations, eliminate
unused channel display-metric passes, move owned IQ blocks without another
full copy, reuse clutter-solver storage, and serialize map/display JSON once.
Qualified integrated GPUs can also use mapped clutter buffers with staged
fallback. FFT math, FP64 host cancellation and startup numerical gates remain
unchanged. The combined campaign does not isolate each change's contribution.

CPU detection/tracking JSON is byte-identical before/after for all six focused
host/workload pairs, with two repeats each. The independent
[serializer microbenchmark](JSON_OUTPUT_OPTIMIZATION_20260911.md) additionally
checks frozen-legacy JSON compatibility; its 23–25% serializer CPU-time
reduction is not a whole-pipeline speedup claim.

## Remaining processing costs

For Strix, two channels, 200 ms CPI and ±2400 Hz, the current GPU pipeline
averages **44.669 ms**, down from **69.650 ms** in the same-campaign prior
VectorWarp run (35.9% less). Actual upstream averages 136.414 ms.

| Timed stage | Prior VectorWarp GPU | Current VectorWarp GPU |
| --- | ---: | ---: |
| JSON output | 23.488 ms | 17.066 ms |
| Clutter filtering | 10.634 ms | 13.283 ms |
| Delay–Doppler / map preparation | 11.167 ms | 6.345 ms |
| Fusion | 2.499 ms | 2.743 ms |
| Spectrum | 3.409 ms | 1.699 ms |
| Input extraction | 2.422 ms | 1.541 ms |
| Detection | 15.372 ms | 1.306 ms |
| Tracking | 0.659 ms | 0.685 ms |
| Complete pipeline | 69.650 ms | **44.669 ms** |

JSON remains the largest measured stage: 17.066 ms, or 38.2% of this pipeline.
Clutter is next at 13.283 ms (29.7%), including the CPU solve and transfers;
delay–Doppler/map preparation is 6.345 ms (14.2%). Detection is now 1.306 ms.
Not every stage improved: clutter is higher than the prior 10.634 ms in this
profile even though total processing is substantially lower. These are stage
timers, not a kernel-only or zero-copy claim. They identify the remaining costs;
no further optimization is claimed or implemented by this report.
The [wider-Doppler algorithm](FUTURE_WIDE_DOPPLER.md) also remains future work.

## Earlier native live processor measurements

The separate [Pi 4 CPU comparison](PI4_PERFORMANCE_20260911.md) includes
Off World Labs' ARM fork. It retains the same excess-path range but is a
separate recording/hardware cohort; its numbers are not pooled with this table.

These historical measurements used the version with recurring CPU accuracy
checks. They were not rerun for the startup-only or combined efficiency changes.
Six short native runs used live Kraken input at 527 MHz and 2.4 MS/s on Strix
(eight physical CPUs, 800% CPU budget), with the same full 256-bin delay range.
Pair rows process two channels selected from the five-channel receiver; only
the array row processes all five channels. Each run has 40 frames with eight
startup frames excluded. Their p95 is the ordinary 32-frame percentile.

These are live ingress and processor-timer evidence, not matched RF comparisons
between modes; the replay tables above remain the direct upstream evidence.

| Live workload | Mode | Mean ms | p95 ms | Max ms | Misses |
| --- | --- | ---: | ---: | ---: | ---: |
| Pair, ±2400 Hz, 200 ms CPI | CPU | 148.562 | 155.363 | 158.890 | 0/32 |
| Pair, ±2400 Hz, 200 ms CPI | GPU | **78.873** | 116.978 | 168.670 | 0/32 |
| Five-channel array, ±800 Hz, 200 ms CPI | CPU | 177.468 | 183.653 | 191.390 | 0/32 |
| Five-channel array, ±800 Hz, 200 ms CPI | GPU | **95.540** | 152.172 | 229.740 | 2/32 |
| Pair, ±4000 Hz, 200 ms CPI | GPU | **113.536** | 161.797 | 226.080 | 2/32 |
| Pair, ±4000 Hz, 1 s CPI | GPU | **564.663** | 789.005 | 1092.320 | 2/32 |

Every live GPU run recorded 30 GPU-backed clutter frames and two CPU-oracle
frames; the forced GPU delay–Doppler stage was active. The processor caught up
from startup backlog, so observed arrival intervals can briefly be shorter than
the requested CPI. No sample/drop counter was available, so these runs do not
prove loss-free acquisition.

## Method, accuracy, and evidence

Each result is two alternating 20-frame repeats. The first eight startup frames
of each repeat are excluded, leaving 24 steady CPIs per row. The DSP is paced
from the recording sample clock, not live RF or browser timing. All **110 timed
runs (2,200 complete CPIs)** passed processing acceptance: **41 paired groups**
form the current matrix, with **14 prior-version groups** for the focused
before/after comparison, totaling 55 groups.

All GPU complex maps stayed within `1e-4` of the independent CPU reference,
including RMS/peak checks before and after fusion. This validates the tested
maps, not bit-exact output or identical detection SNR across CPU/GPU modes.
The benchmark's independent CPU comparisons remain enabled outside the timed
pipeline. Production retains three initial and five composed-stage startup
qualification frames, every-frame finite/precision/error/timeout protections,
CPU fallback and sustained AUTO cost selection. It does not periodically
recompute CPU clutter or complex maps on accepted steady GPU frames, or
continuously revalidate later signal conditions against a full CPU reference.
Each repeat consumes two seconds for a 100 ms CPI, four seconds for a 200 ms
CPI, or twenty seconds for a one-second CPI. These short runs are not an
endurance test.

The current source freeze is `8ba6e1330fad9fcdbe8a5a54134a712d0959775e`,
archive SHA-256
`ccbd2ec1c379310b0df1be1857570d93dd5306a6b435c63c8e785b7c8084dab6`.
GPU work covers clutter FFT/filtering and delay–Doppler; the small FP64 clutter
coefficient solve, capture, reference synthesis, detection, tracking and output
handling remain CPU work.

The complete current and before/after summaries are in
[`comparison.csv`](benchmarks/20260911-efficiency/comparison.csv) and
[`comparison.json`](benchmarks/20260911-efficiency/comparison.json).
[Exact settings, hardware, CPU limits and reproduction](benchmarks/20260911-efficiency/README.md)
accompany the per-frame receipts. The
[preceding fixed-range campaign](benchmarks/20260911-equal-range/README.md)
and [earlier mixed-window results](benchmarks/20260911/) remain historical
evidence, not current comparison rows. Exact wider-Doppler CAF at the full
delay range remains future work, not a benchmark result.
