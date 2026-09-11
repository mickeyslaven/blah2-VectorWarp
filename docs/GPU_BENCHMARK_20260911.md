# GPU comparison — 11 September 2026

## Result

This is the current, like-for-like replay comparison. Each matched pair uses
the same samples from one 20.002-second recording (SHA-256
`1e8d50a5fe62410ead9094d95a57af1d03414e87ac00b8861b75444c7aab12aa`), at
527 MHz and 2.4 MS/s, with delays −10…245 (256 bins, maximum excess path
30.604 km) and clutter −10…200. GPU acceleration substantially reduces the
tested pipeline processing time; CPU-only differences are usually small.

The direct comparison below uses the original DSP from
[`30hours/blah2` `c821bee3`](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de).
All figures are ms/CPI (lower is better). P95 is pooled across the 24 steady
CPIs for that row; misses are steady CPI deadlines.

| Host and two-channel workload | CPI | Regular blah2 mean / p95 / misses | VectorWarp CPU mean / p95 / misses | VectorWarp GPU mean / p95 / misses |
| --- | ---: | --- | --- | --- |
| Strix, ±800 Hz | 200 ms | 79.775 / 83.396 / 0/24 | 79.669 / 82.014 / 0/24 | **39.915 / 82.630 / 0/24** |
| Strix, ±2400 Hz | 200 ms | 137.802 / 140.530 / 0/24 | 135.374 / 142.546 / 0/24 | **77.268 / 137.339 / 0/24** |
| Strix, ±800 Hz | 100 ms | 43.126 / 45.590 / 0/24 | 41.538 / 43.610 / 0/24 | **22.439 / 42.416 / 0/24** |
| RTX 4050 Laptop, ±800 Hz | 200 ms | 218.679 / 234.160 / 23/24 | 210.487 / 223.833 / 23/24 | **109.153 / 232.294 / 2/24** |
| RTX 4050 Laptop, ±2400 Hz | 200 ms | 345.106 / 377.519 / 24/24 | 337.309 / 371.747 / 24/24 | **236.549 / 454.478 / 23/24** |
| Pavilion Intel HD 630, ±800 Hz | 200 ms | 204.727 / 208.260 / 24/24 | 204.131 / 206.111 / 24/24 | **115.533 / 236.249 / 2/24** |
| Pavilion AMD Polaris 12, ±800 Hz | 200 ms | 204.727 / 208.260 / 24/24 | 204.131 / 206.111 / 24/24 | **106.998 / 228.246 / 2/24** |
| Pavilion Intel HD 630, ±2400 Hz | 200 ms | 309.947 / 329.557 / 24/24 | 301.840 / 308.012 / 24/24 | **179.449 / 324.859 / 2/24** |
| Pavilion AMD Polaris 12, ±2400 Hz | 200 ms | 309.947 / 329.557 / 24/24 | 301.840 / 308.012 / 24/24 | **174.882 / 320.329 / 2/24** |

## Strix capacity at the same full delay range

These are VectorWarp-only workloads: upstream was not timed where its geometry
is unsafe or it has no equivalent five-channel array mode. They remain the same
527 MHz / 2.4 MS/s / −10…245 / 256-bin / 30.604 km cohort.

| Workload | CPI | VectorWarp CPU mean / p95 / misses | VectorWarp GPU mean / p95 / misses |
| --- | ---: | --- | --- |
| Two-channel pair, ±4800 Hz | 200 ms | 231.237 / 234.783 / 24/24 | **127.858 / 227.881 / 2/24** |
| Five-channel array, ±800 Hz | 200 ms | 175.428 / 179.069 / 0/24 | **98.963 / 200.895 / 2/24** |
| Two-channel pair, ±4800 Hz | 1 s | 1192.768 / 1212.540 / 24/24 | **650.484 / 1185.861 / 2/24** |
| Five-channel array, ±2400 Hz | 1 s | 1418.515 / 1442.808 / 24/24 | **807.401 / 1580.066 / 2/24** |

## Older GPUs: same-range capacity

The RTX 4050 Laptop campaign used four physical CPU cores; the Pavilion used
its available Intel HD 630 and AMD Polaris 12 Vulkan devices. These rows are
VectorWarp-only capacity measurements, so they are not presented as upstream
speedups.

| Host and workload | CPI | VectorWarp CPU mean / p95 / misses | VectorWarp GPU mean / p95 / misses | GPU delay–Doppler / clutter frames |
| --- | ---: | --- | --- | --- |
| RTX 4050 Laptop pair, ±4800 Hz | 200 ms | 533.311 / 598.044 / 24/24 | **361.641 / 604.565 / 24/24** | 22/24 / 24/24 |
| RTX 4050 Laptop five-channel array, ±800 Hz | 200 ms | 550.953 / 577.119 / 24/24 | **326.723 / 607.102 / 24/24** | **16/24** / 24/24 |
| Pavilion Intel HD 630 pair, ±4800 Hz | 200 ms | 482.085 / 486.700 / 24/24 | **284.123 / 496.815 / 24/24** | 22/24 / 24/24 |
| Pavilion AMD Polaris 12 pair, ±4800 Hz | 200 ms | 482.085 / 486.700 / 24/24 | **279.698 / 494.282 / 24/24** | 22/24 / 24/24 |
| Pavilion Intel HD 630 five-channel array, ±800 Hz | 200 ms | 568.039 / 582.264 / 24/24 | **322.726 / 686.567 / 24/24** | 22/24 / 24/24 |
| Pavilion AMD Polaris 12 five-channel array, ±800 Hz | 200 ms | 568.039 / 582.264 / 24/24 | **285.875 / 642.224 / 24/24** | 22/24 / 24/24 |

The NVIDIA array's Automatic mode used GPU clutter on all 24 steady CPIs but
GPU delay–Doppler on 16/24; it is reported as measured, not as a fully GPU
delay–Doppler result. The other Automatic GPU rows used 22 GPU delay–Doppler
CPIs and two periodic CPU-oracle CPIs. Those oracle checks mean a low average
does not guarantee that every interval completes on time.

## Native live processor

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

## What made it faster

The main gain in these tests comes from the combined GPU path, not CPU-only
changes. For the Strix two-channel, 200 ms, ±2400 Hz workload:

| Processing stage | VectorWarp CPU | VectorWarp GPU mode |
| --- | ---: | ---: |
| Clutter filtering | 21.914 ms | 12.555 ms |
| Delay–Doppler | 62.341 ms | 15.887 ms |
| Complete pipeline | 135.374 ms | 77.268 ms |

These averages include the periodic CPU accuracy checks. Shared inputs, batched
GPU work and lower-copy transfers are implemented, but this campaign does not
separately measure the contribution of each optimization.

The remaining CPU work is worth investigating next: map JSON output averaged
24.344 ms and detection 15.366 ms in that GPU profile. Reducing output conversion
cost and checking whether accuracy verification can run without blocking the
next frame are future optimization candidates, not demonstrated improvements.
The [wider-Doppler algorithm](FUTURE_WIDE_DOPPLER.md) is also deferred.

## Method, accuracy, and evidence

Each replay result is two alternating 20-frame repeats. The first eight startup
frames of each repeat are excluded, leaving 24 steady CPIs per row. The DSP is
paced from the recording sample clock, not live RF or browser timing. The 82
timed runs (41 paired groups, 1,640 complete CPIs) passed processing acceptance;
all GPU complex maps were within `1e-4` of the CPU reference. That validates
the tested maps, not bit-exact output or identical detection SNR.
Each repeat consumes two seconds for a 100 ms CPI, four seconds for a 200 ms
CPI, or twenty seconds for a one-second CPI. These short runs are not an
endurance test.

The campaign used the v8 source freeze
`8ceb110d4b71854a653789696b17fd7a57a318ab58f6c27212f9899422b666c3`.
Its expanded allocation cap did not change DSP math from v6. GPU work covers
clutter FFT/filtering and delay–Doppler; the small FP64 clutter coefficient
solve, capture, reference synthesis, detection, tracking and output handling
remain CPU work.

The complete 41-group summaries and provenance are retained in
[`docs/benchmarks/20260911-equal-range/comparison.csv`](benchmarks/20260911-equal-range/comparison.csv)
and [`comparison.json`](benchmarks/20260911-equal-range/comparison.json).
[Exact settings, hardware, CPU limits and reproduction](benchmarks/20260911-equal-range/README.md)
accompany every per-frame receipt.
Earlier mixed-window results remain available as
[archival evidence](benchmarks/20260911/); they are not used in any current
comparison table. Exact wider-Doppler CAF at the full delay range is future
work, not a benchmark result.
