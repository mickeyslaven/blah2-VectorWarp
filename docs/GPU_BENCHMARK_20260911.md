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
| Strix, ±800 Hz | 200 ms | 80.526 / 83.511 / 0/24 | 79.557 / 83.755 / 0/24 | **38.539 / 43.526 / 0/24** |
| Strix, ±2400 Hz | 200 ms | 135.308 / 140.326 / 0/24 | 132.219 / 139.336 / 0/24 | **70.458 / 73.149 / 0/24** |
| Strix, ±800 Hz | 100 ms | 42.591 / 44.801 / 0/24 | 42.499 / 46.006 / 0/24 | **23.257 / 25.507 / 0/24** |
| RTX 4050 Laptop, ±800 Hz | 200 ms | 225.574 / 261.179 / 23/24 | 220.020 / 241.017 / 22/24 | **112.968 / 146.959 / 0/24** |
| RTX 4050 Laptop, ±2400 Hz | 200 ms | 338.925 / 370.456 / 24/24 | 334.922 / 384.891 / 24/24 | **211.915 / 236.245 / 22/24** |
| Pavilion Intel HD 630, ±800 Hz | 200 ms | 205.172 / 207.411 / 24/24 | 204.815 / 208.980 / 24/24 | **101.340 / 105.986 / 0/24** |
| Pavilion AMD Polaris 12, ±800 Hz | 200 ms | 205.172 / 207.411 / 24/24 | 204.815 / 208.980 / 24/24 | **94.841 / 101.504 / 0/24** |
| Pavilion Intel HD 630, ±2400 Hz | 200 ms | 310.520 / 332.024 / 24/24 | 303.218 / 306.132 / 24/24 | **165.852 / 171.800 / 0/24** |
| Pavilion AMD Polaris 12, ±2400 Hz | 200 ms | 310.520 / 332.024 / 24/24 | 303.218 / 306.132 / 24/24 | **157.501 / 158.585 / 0/24** |

## Strix capacity at the same full delay range

These are VectorWarp-only workloads: upstream was not timed where its geometry
is unsafe or it has no equivalent five-channel array mode. They remain the same
527 MHz / 2.4 MS/s / −10…245 / 256-bin / 30.604 km cohort.

| Workload | CPI | VectorWarp CPU mean / p95 / misses | VectorWarp GPU mean / p95 / misses |
| --- | ---: | --- | --- |
| Two-channel pair, ±4800 Hz | 200 ms | 230.185 / 236.385 / 24/24 | **117.240 / 120.332 / 0/24** |
| Five-channel array, ±800 Hz | 200 ms | 174.950 / 179.930 / 0/24 | **89.388 / 93.132 / 0/24** |
| Two-channel pair, ±4800 Hz | 1 s | 1175.055 / 1190.119 / 24/24 | **587.160 / 592.492 / 0/24** |
| Five-channel array, ±2400 Hz | 1 s | 1411.773 / 1439.856 / 24/24 | **714.872 / 726.324 / 0/24** |

## Older GPUs: same-range capacity

The RTX 4050 Laptop campaign used four physical CPU cores; the Pavilion used
its available Intel HD 630 and AMD Polaris 12 Vulkan devices. These rows are
VectorWarp-only capacity measurements, so they are not presented as upstream
speedups.

| Host and workload | CPI | VectorWarp CPU mean / p95 / misses | VectorWarp GPU mean / p95 / misses | GPU delay–Doppler / clutter frames |
| --- | ---: | --- | --- | --- |
| RTX 4050 Laptop pair, ±4800 Hz | 200 ms | 513.808 / 574.362 / 24/24 | **336.965 / 360.754 / 24/24** | 24/24 / 24/24 |
| RTX 4050 Laptop five-channel array, ±800 Hz | 200 ms | 594.061 / 623.846 / 24/24 | **264.195 / 298.467 / 24/24** | **17/24** / 24/24 |
| Pavilion Intel HD 630 pair, ±4800 Hz | 200 ms | 481.153 / 492.768 / 24/24 | **262.696 / 269.221 / 24/24** | 24/24 / 24/24 |
| Pavilion AMD Polaris 12 pair, ±4800 Hz | 200 ms | 481.153 / 492.768 / 24/24 | **258.156 / 265.001 / 24/24** | 24/24 / 24/24 |
| Pavilion Intel HD 630 five-channel array, ±800 Hz | 200 ms | 564.084 / 573.625 / 24/24 | **281.021 / 289.367 / 24/24** | 22/24 / 24/24 |
| Pavilion AMD Polaris 12 five-channel array, ±800 Hz | 200 ms | 564.084 / 573.625 / 24/24 | **246.557 / 248.690 / 24/24** | 24/24 / 24/24 |

Every Automatic GPU row used GPU clutter on all 24 steady CPIs, with no CPU
accuracy oracles. GPU delay–Doppler ran on 24/24 except the NVIDIA five-channel
array (17/24) and Intel five-channel array (22/24). Their retained AUTO cost
selection chose CPU ambiguity for the remaining frames; these are mixed-stage
results, not fully GPU delay–Doppler results. Heavier workloads still miss CPI
deadlines, as shown above; removing accuracy recomputation is not a universal
real-time guarantee.

## Earlier native live processor measurements

These historical measurements used the version with recurring CPU accuracy
checks. They were not rerun for the startup-only change. Six short native runs
used live Kraken input at 527 MHz and 2.4 MS/s on Strix
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
| Clutter filtering | 21.867 ms | 10.861 ms |
| Delay–Doppler | 59.454 ms | 11.421 ms |
| Complete pipeline | 132.219 ms | 70.458 ms |

These replay averages exclude startup qualification and contain no recurring
CPU accuracy oracles. Shared inputs, batched
GPU work and lower-copy transfers are implemented, but this campaign does not
separately measure the contribution of each optimization.

The remaining CPU work is worth investigating next: map JSON output averaged
23.501 ms and detection 15.486 ms in that GPU profile. Reducing output conversion
cost is a future optimization candidate, not a demonstrated improvement.
The [wider-Doppler algorithm](FUTURE_WIDE_DOPPLER.md) is also deferred.

## Method, accuracy, and evidence

Each replay result is two alternating 20-frame repeats. The first eight startup
frames of each repeat are excluded, leaving 24 steady CPIs per row. The DSP is
paced from the recording sample clock, not live RF or browser timing. The 82
timed runs (41 paired groups, 1,640 complete CPIs) passed processing acceptance;
all GPU complex maps were within `1e-4` of the CPU reference. That validates
the tested maps, not bit-exact output or identical detection SNR.
The benchmark's independent CPU comparisons remain enabled outside the timed
pipeline. Production retains three initial and five composed-stage startup
qualification frames, every-frame finite/precision/error/timeout protections,
CPU fallback and sustained AUTO cost selection. It does not periodically
recompute CPU clutter or complex maps on accepted steady GPU frames, or
continuously revalidate later signal conditions against a full CPU reference.
Each repeat consumes two seconds for a 100 ms CPI, four seconds for a 200 ms
CPI, or twenty seconds for a one-second CPI. These short runs are not an
endurance test.

The campaign used the startup-only source freeze
`2239b08347ef1b2466e7f9a10a269ed397939cc3bcea2aa5acc29d68b32fc9bc`,
with the qualification change integrated as `9039d73`. FFT math and startup
numerical gates are unchanged from v8; recurring CPU accuracy recomputation was
removed. GPU work covers
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
