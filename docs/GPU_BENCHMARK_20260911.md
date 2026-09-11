# GPU comparison — 11 September 2026

## Result

On an i7-12650H laptop with a 6 GiB RTX 4050 Laptop GPU, VectorWarp's Vulkan
path reduced the standard matched workload from **240.118 ms/CPI** in regular
`30hours/blah2` to **127.770 ms/CPI**: **46.8% less processing time**. The
VectorWarp CPU result was 239.479 ms/CPI, essentially the same as upstream in
this workload.

| 200 ms CPI workload | Regular blah2 CPU | VectorWarp CPU | VectorWarp GPU | Steady deadline misses, upstream / CPU / GPU |
| --- | ---: | ---: | ---: | ---: |
| Pair, ±800 Hz | 240.118 | 239.479 | **127.770** | 23/24 / 23/24 / 2/24 |
| Pair, ±2400 Hz | 341.780 | 347.133 | **234.069** | 24/24 / 24/24 / 20/24 |
| Pair, ±4000 Hz | Not safely supported | 487.264 | **319.268** | n/a / 24/24 / 24/24 |
| Five-channel array, ±800 Hz | No equivalent upstream mode | 549.386 | **302.140** | n/a / 24/24 / 20/24 |

All figures are milliseconds per complete CPI; lower is better. The standard
row is the direct upstream comparison. The ±4000 Hz and five-channel rows show
useful VectorWarp capacity, but are not speedups over blah2: upstream was not
run where its geometry was unsafe or where it has no equivalent array mode.

The standard GPU result is encouraging, but it is not a 100 ms real-time claim:
its mean is about 128 ms and it still missed two of 24 measured 200 ms deadlines.
The wide and array workloads also missed deadlines at this CPU allocation.

## Pavilion: two older GPUs, same upstream baseline

The completed Pavilion campaign repeated the paired replay on both available
Vulkan devices: Intel HD Graphics 630 (KBL GT2) and AMD Radeon 500 Series
(RADV POLARIS12). The direct comparison remains regular upstream blah2, not a
VectorWarp CPU-only build.

| 200 ms CPI workload | Regular blah2 CPU | VectorWarp CPU | Intel GPU | AMD GPU |
| --- | ---: | ---: | ---: | ---: |
| Pair, ±800 Hz mean | 204.887 | 204.343 | **114.540** | **108.215** |
| Pair, ±800 Hz p95 | 208.465 | 207.135 | 175.390 | 168.061 |
| Pair, ±800 Hz misses | 24/24 | 24/24 | 2/24 | 2/24 |
| Pair, ±2400 Hz mean | 308.096 | 303.164 | **183.244** | **174.629** |
| Pair, ±2400 Hz p95 | 312.916 | 309.900 | 256.497 | 246.385 |
| Pair, ±2400 Hz misses | 24/24 | 24/24 | 2/24 | 2/24 |

These are the means of two alternating 12-frame steady windows, in ms/CPI.
The GPUs saved about 41–47% on the standard pair and 41–43% on the wider pair
relative to actual upstream blah2. A periodic CPU oracle occupied one of each
12-frame GPU steady window; it explains the two deadline misses and should not
be described as a continuous GPU-only real-time result.

The campaign also measured VectorWarp-only capacity. At ±4000 Hz, Intel/AMD GPU
means were 253.263/247.161 ms/CPI (all 24 deadlines missed); the CPU mean was
442.701 ms/CPI. For the five-channel ±800 Hz array, Intel/AMD GPU means were
321.475/285.746 ms/CPI, compared with 563.478 ms/CPI CPU; all were over the
200 ms deadline. Upstream has no equivalent array mode and was excluded from
±4000 Hz because its Doppler scratch geometry is unsafe.

For the Pavilion's active sensor readings, the peak CPU package temperature was
83°C against a 100°C critical limit and the active AMD edge sensor peaked at
54°C against 94°C. The guard treats an initially runtime-suspended AMD sensor
as unavailable, rather than accepting a false zero; once the device was active,
every recorded guard observation was fresh. This is bounded campaign evidence,
not an endurance or thermal-margin claim.

## Strix: matched replay with more CPU capacity

Strix used eight physical CPUs (0–7), an 800% CPU budget and 12 GiB memory.
These results are useful capacity evidence for that host; they are not a
cross-host ranking of GPUs or CPUs.

| 200 ms CPI workload | Regular blah2 CPU | VectorWarp CPU | VectorWarp GPU | GPU deadline misses |
| --- | ---: | ---: | ---: | ---: |
| Pair, ±800 Hz | 84.948 | 78.041 | **41.666** | 0/24 |
| Pair, ±2400 Hz | 138.792 | 132.946 | **74.856** | 0/24 |
| Five-channel array, ±800 Hz | No equivalent upstream mode | 175.815 | **100.995** | 2/24 |

Pair runs used one worker/eight FFT threads; the array used four workers/two
FFT threads. The same input, source baseline and 12-frame steady-window method
apply. GPU maps passed the same 1e-4 relative RMS/peak acceptance tolerance;
they are not bit-exact outputs and this timing campaign does not claim identical
detection SNR.

## Wide-Doppler capacity

The Strix v8 campaign demonstrates computational capacity for much wider
Doppler windows using the same recorded 527 MHz, 2.4 MS/s IQ. It is not a
measurement of satellite-TV, LEO, Starlink or Ku-band reception, and it does
not add RF bandwidth, a higher-frequency frontend, moving-illuminator
compensation, a link budget, or aircraft-detection evidence. Doppler span and
RF carrier frequency/bandwidth are different quantities.

| Workload | VectorWarp CPU mean | VectorWarp GPU mean | GPU p95 | GPU misses |
| --- | ---: | ---: | ---: | ---: |
| ±40 kHz, 50 ms CPI | 187.646 ms | **46.135 ms** | 107.772 ms | 2/24 |
| ±40 kHz, 100 ms CPI | 372.311 ms | **87.368 ms** | 209.785 ms | 2/24 |
| ±40 kHz, 200 ms CPI | 740.301 ms | **170.719 ms** | 410.317 ms | 2/24 |
| ±40 kHz, 500 ms CPI | 1862.562 ms | **419.176 ms** | 1009.335 ms | 2/24 |
| ±20 kHz, 1 s CPI | 2580.833 ms | **769.310 ms** | 1535.162 ms | 2/24 |

Each row is a two-repeat, 24-frame steady aggregate. Eleven frames in each
12-frame GPU window use GPU clutter and ambiguity processing; one periodic CPU
oracle checks accuracy. That oracle creates the two deadline misses, so a mean
below the CPI is useful capacity evidence, not a guarantee that every frame
will meet cadence. Regular upstream blah2 was excluded: its geometry is unsafe
for all one-second stress profiles, so no upstream timing is invented here.

The wide spans deliberately trade range gates for processing size. ±40 kHz uses
32 delay bins from −10 to 21, up to **2.623 km** excess path. ±20 kHz uses 64
bins from −10 to 53, up to **6.620 km**. The 1 s ±2400 Hz full-coverage pair
retains 256 bins (up to **30.604 km** excess path) and measured 551.091 ms GPU
versus 1005.935 ms VectorWarp CPU; its GPU p95 was 776.886 ms with 2/24 misses.

Higher carrier frequencies cause proportionally larger Doppler shifts for the
same relative motion. That makes the demonstrated headroom relevant to future
experiments with higher-frequency illuminators, including satellite-TV and LEO
downlinks. It is only a processing result. For broader passive-radar context,
see the [Fraunhofer publication](https://publica.fraunhofer.de/entities/publication/9f079ad1-1f9e-4f87-a228-eeb7a5e67332).

## What was compared

- **Baseline:** unmodified `30hours/blah2` commit
  [`c821bee3f0d27cf20c8447f3d908ef722905a4de`](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de), not a Kraken pull request.
- **VectorWarp sources:** experimental v6 source freeze SHA-256
  `4de77e6f9b7ec8afa490e30c287f356fbe6f669d5b770f7d85c3071c37abcd08`.
  The v7 allocation-guard-only follow-up freeze has SHA-256
  `1dd92939ab7d735074f27a1c31a93780f776e419106adca3b43380125b32c488`;
  its math is unchanged.
  The corrected GPU source commit was
  `407b9737c744f2ef754139767adad238d25d52f0`; the v7 kernel math is unchanged.
- **Host and limits:** i7-12650H, RTX 4050 Laptop GPU (6 GiB), four physical
  P-cores (0, 2, 4 and 6), 400% CPU allowance, 3 GiB memory limit and no swap.
  The guarded run used a 78°C stop threshold.
- **Input:** the same four seconds of real five-channel recorded IQ for every
  compared run, SHA-256
  `89bfe1c338995167946b7c367b80480143b60a515d9eabde39380bcc48e03a5d`.
  The processor selected physical pair 0/1; the array case used all five
  channels with a synthesized reference.
- **Geometry:** 527 MHz, 2.4 MS/s, 200 ms CPI, delays −10…245 (256 bins), and
  clutter −10…200. The standard, wide and extra-wide profiles use ±800, ±2400
  and ±4000 Hz Doppler respectively. Pair runs use one worker/four FFT threads;
  the array uses four workers/one FFT thread.
- **Method:** two alternating repeats per mode, 20 frames each. The first eight
  startup frames were excluded for every engine, leaving 12 steady frames per
  repeat and 24 per result. DSP was paced from the recording's sample clock;
  it is not live RF acquisition or browser timing.

## GPU scope and accuracy

The GPU accelerates delay–Doppler processing. Clutter FFTs run on the GPU, but
the clutter FP64 solve remains on the CPU; extraction, reference work,
detection, tracking and output handling also remain CPU work. The implementation
uses shared-host/staging transfers, not an all-GPU or zero-copy pipeline.

Each forced-GPU repeat produced 11 GPU-backed steady frames and one periodic
CPU oracle frame. The campaign's full 28-row Pavilion cohort passed its output checks;
the recorded GPU maps were compared against the CPU reference before they were
used. This validates the tested data and tolerances, not every GPU, driver,
signal or operating condition.

The array GPU runs varied substantially between repeats (334 ms and 270 ms
steady means). The table reports their mean rather than the faster repeat.

## Interpretation

This is a matched, resource-limited processing comparison, not a live radar or
endurance test. Both engines replayed the same captured samples at their
original rate. When a run exceeded its CPI budget, work accumulated as lag
rather than silently dropping input. The report therefore supports the stated
per-CPI and deadline results, not a claim about loss-free reception, aircraft
detection quality, browser performance, or universal GPU speedups.

Automatic mode measures the complete GPU ambiguity path and can retain CPU
processing when GPU work does not sustain its margin. Forced GPU mode is useful
for verification; it is not a recommendation to override Automatic mode on an
unmeasured host. See [GPU acceleration](GPU_ACCELERATION.md) for configuration
and fallback behavior.

## Evidence retained with the campaign

The source evidence is the 20-row NVIDIA v6 result cohort at
`/var/tmp/vectorwarp-combined-results-20260911.wH4BQf/nvidia-v6/results/summary.json`,
with per-frame CSV files, commands, geometries and exclusions beside it.
Precision, thermal and preflight receipts remain in the parent directory. This
repository records the human-readable result; the machine-local evidence retains
the raw timing and command receipts.

The publishable Pavilion aggregate, profiles and provenance are checked in at
[`docs/benchmarks/20260911`](benchmarks/20260911/).
