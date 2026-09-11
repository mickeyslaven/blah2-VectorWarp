# Live and sample-clock-paced capacity — 10 September 2026

Historical development evidence, superseded for performance claims by the
[current fixed-range report](GPU_BENCHMARK_20260911.md), including its separately
identified native-live results.

VectorWarp kept pace in short live five-channel and wide-Doppler runs on Strix.
GPU acceleration also reduced processing time against unchanged upstream DSP
in matched, sample-clock-paced replay. Neither result means GPU is universally
faster or that every configuration will run in real time.

## Actual live processor: Strix

Ryzen AI Max+ 395, Radeon 8060S (RADV), Fedora; stock frequency policy, eight
physical cores available (0–7), 800% CPU allowance, 12 GiB memory maximum,
no swap. Real five-input Kraken Suite V2: 527 MHz, 2.4 MS/s, coherent mode.
Pair rows select physical channels 0/1; array rows synthesize the reference
from all five inputs and compute/fuse five independent surveillance maps.
Pair processing uses one worker/four FFT threads; arrays use five workers/one
FFT thread each. OpenBLAS and OpenMP are limited to one thread throughout.

All configurations use delays −10…245 (256 bins), clutter −10…200, detection
and tracking enabled, zero overlap. The exact configuration for every run is
in [live-strix.json](benchmarks/20260910/live-strix.json).

| Workload | Deadline | CPU mean / p95 ms | GPU mean / p95 ms | CPU / GPU misses |
| --- | ---: | ---: | ---: | ---: |
| Pair, ±1600 Hz | 100 ms | 52.05 / 55.24 | 42.58 / 44.43 | 0/27 / 0/27 |
| Pair, ±2400 Hz | 200 ms | 116.87 / 120.88 | 100.04 / 104.11 | 0/27 / 0/27 |
| Five-channel array, ±800 Hz | 200 ms | 147.53 / 151.05 | 158.02 / 160.72 | 0/27 / 0/27 |
| Pair, ±4000 Hz | 200 ms | 167.30 / 173.83 | 138.70 / 142.70 | 0/27 / 0/27 |
| Five-channel array, ±1600 Hz | 400 ms | 368.16 / 378.78 | 427.68 / 437.17 | 0/27 / 27/27 |

Ten runs, 300 CPIs total; each has 30 frames with the first three excluded.
The GPU retained Vulkan for all 27 measured frames in every forced-GPU case.
Those three initial CPU-owned frames independently check GPU maps before use.
CPU and GPU runs use sequential arriving RF, **not identical IQ**. No live
upstream executable was run; do not label this table an upstream speedup.

The measured arrival cadence was approximately 10, 5 or 2.5 CPIs/s as requested
for passing cases. The 400 ms five-channel GPU case produced only 2.34 CPIs/s.
Timing comes directly from the native processor's TCP timing output, not the
browser: extraction, reference synthesis, spectrum, clutter, ambiguity, fusion,
detection/tracking and radar-output serialization/transport are included.
Waiting for incoming samples, initialization, final timing/status publication
and browser rendering are outside the `cpi` timer.

The valid ±4000 Hz case has 1601 Doppler bins and a 600-element range FFT.
Regular blah2 allocates its Doppler scratch buffer to the range FFT length and
cannot safely represent this case. VectorWarp separates the buffer dimensions.
Here it both supports the larger geometry **and** met every measured 200 ms
processing deadline. This is a configuration-specific limit, not a universal
maximum Doppler frequency. Delay limits outside the correlation block are
rejected; the previously excluded ±6000 Hz / full-delay case remains invalid.

No recording or ADS-B service was started for these live runs. MCHQ has no
hardware sample sequence counter, and the live capture queues do not expose
drop totals: timing/cadence alone cannot prove loss-free acquisition. These
are short 3–12-second signal windows per run, not endurance tests.

Thermal context: an earlier two-CPU trial completed four pair runs, then
stopped the array run at its conservative 78°C cutoff. It is not pooled with
this table. The owner then authorized stock-clock testing with eight cores and
a 100°C benchmark ceiling. The measurement helper used that ceiling, but the
legacy 82°C native guard also remained active through the existing receiver
dependency. The completed set peaked at **81.5°C**, below both; no hardware
throttle-onset or 100°C stability claim is supported. Configuration bytes and
all CPU frequency limits were unchanged afterward; receiver/processor were
returned to their paused state. Automatic fans and global protection stayed on.

## Identical recorded IQ, released at its live sample rate

The benchmark adapter compiles all twelve DSP/data sources from actual
`30hours/blah2` commit `c821bee3f0d27cf20c8447f3d908ef722905a4de`, unchanged.
All twelve compiled source files were hash-compared against that commit.
Both engines use the same MCHQ input preparation, compiler, DSP libraries,
profile, physical reference/surveillance pair and four FFT threads. Each
complete CPI is released on its original sample clock; overload accumulates
lag instead of dropping input. This is **not upstream native acquisition**.

Strix: same eight-core/800% allowance as above. NVIDIA laptop: i7-12650H and
RTX 4050 Laptop, four physical P-cores (0,2,4,6), 400% allowance, 6 GiB/no swap.
CPU/GPU limits are identical within each host, not between hosts. Both paced
engines use identical Ubuntu DSP libraries; the separate native live processor
uses Fedora libraries, so do not subtract timings across those two methods.

Two repeats per configuration/mode; second repeat reverses engine order.
Each repeat uses the same four seconds of real five-channel recorded IQ,
selecting channels 0/1. The first three frames are excluded from each mean.

| Host / profile | Regular blah2 CPU | VectorWarp CPU | VectorWarp GPU | Warm deadline misses, upstream / CPU / GPU |
| --- | ---: | ---: | ---: | ---: |
| Strix, 200 ms / ±800 Hz | 76.10 ms | 74.46 ms | 61.95 ms | 0/34 / 0/34 / 0/34 |
| Strix, 250 ms / ±2000 Hz | 133.32 ms | 128.55 ms | 109.72 ms | 0/26 / 0/26 / 0/26 |
| RTX 4050 laptop, 200 ms / ±800 Hz | 238.84 ms | 220.50 ms | 162.29 ms | 33/34 / 29/34 / 0/34 |
| RTX 4050 laptop, 250 ms / ±2000 Hz | 380.68 ms | 385.91 ms | 299.72 ms | 26/26 / 26/26 / 26/26 |

Values are the mean of two equal-length warm-run means, not best-frame times.
The NVIDIA 200 ms GPU case used **32.1% less processing time** than upstream;
its two run means were 165.34 and 159.23 ms, versus 252.53 and 225.15 ms upstream.
The GPU's final scheduled-completion lag was 45 ms and 0 ms, versus 1527 ms and
1068 ms upstream. Startup qualification and read/validation overhead can create
schedule debt even when all subsequent DSP timings fit the deadline. Strix
ended all matched runs with zero schedule debt. The heavier NVIDIA profile
ended behind schedule in every mode; it is not a real-time success.

These 24 runs contain 432 measured CPIs (including initial frames). Every
compared complex map passed the 1e-4 relative RMS/peak tolerance. CPU maps
matched upstream exactly in this sample; GPU maps passed the tolerance.
Detection SNR, edge handling and tracking corrections are documented in the
[earlier comparison](PER_CPI_BENCHMARK_20260910.md); full output equivalence is
not claimed. These tests measure speed/capacity, not improved aircraft detection.

## What actually helps

- GPU delay–Doppler processing provides the demonstrated matched-pair speedup.
  It does not accelerate the entire pipeline; clutter filtering remains CPU work.
- Channel workers distribute independent five-channel work. The earlier
  controlled worker ablation measured 47% less processing time versus the
  sequential five-channel path; the live array now fits its 200 ms deadline
  on Strix. This is not proof that five channels cost the same as two.
- Separate Doppler scratch storage provides broader valid configuration
  support, demonstrated here with a live ±4000 Hz case.
- CPU-only results remain near parity, variable, or slower across the full
  campaign. Forced GPU can lose to parallel CPU; AUTO can fall back.

Next optimization candidates—not implemented or claimed as gains here—are
reusing reference transforms across channel filters, efficient FFT lengths in
clutter processing, reducing repeated IQ/map copies, and stage-specific thread
sizing. Each needs numerical regression checks and its own matched ablation.

## Evidence and reproduction

- [Live configurations and summaries](benchmarks/20260910/live-strix.json),
  [300 native per-CPI timings](benchmarks/20260910/live-per-cpi.csv).
- [Paced configurations, binary hashes and 24 summaries](benchmarks/20260910/paced-comparison.json),
  [432 paced per-CPI timings](benchmarks/20260910/paced-per-cpi.csv).
- [Benchmark build and pacing instructions](../bench/README.md).
- Native base source: `f1345ba84a3f0da5efa505747dc884ae27b2c5b6` plus the
  signed-delay validation/extraction patch, whose SHA-256 is in the capsules.
- Recorded window SHA-256:
  `89bfe1c338995167946b7c367b80480143b60a515d9eabde39380bcc48e03a5d`.

Full machine-local commands, configurations, logs, temperatures and cleanup
receipts are retained under `/var/tmp/vectorwarp-strix-stock-20260911.2iUvmB`.
The preliminary guarded trial is separately retained under
`/var/tmp/vectorwarp-strix-live-20260911.7ZcGVA`.
