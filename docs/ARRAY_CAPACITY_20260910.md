# Five-channel capacity and wider Doppler — 10 September 2026

The channel-worker changes measurably reduce five-channel CPU processing time.
They do **not** establish five-channel processing at the cost of regular blah2's
two-channel pair. VectorWarp also completes wider-Doppler configurations that
the pinned upstream implementation cannot safely execute, but the tested wide
profiles still exceed their 200 ms processing budget.

## Controls and scope

This extends the [per-CPI campaign](PER_CPI_BENCHMARK_20260910.md), using the
same Intel i7-12650H / RTX 4050 Laptop on Fedora 43 and NVIDIA 580.173.02.
The same binaries, DSP libraries and verified real 527 MHz, 2.4 MS/s MCHQ
recording were reused. Upstream is actual `30hours/blah2` main at `c821bee`,
not the Kraken PR. VectorWarp DSP is frozen at `813618b`; the later benchmark,
UI and documentation commits through `61a3c2f` do not change that arithmetic.
Source/binary/input hashes are retained in the data and earlier report.

All cases use 200 ms input CPIs, no overlap, delay −10…245 (256 bins), clutter
−10…200 (210 taps), the same detector/tracker settings and Hamming-number FFT
rounding. The two-channel profile processes reference 0 and surveillance 1.
The five-channel profile synthesizes its reference from channels 0–4, computes
**five surveillance maps**, then performs noncoherent magnitude fusion. It is
not just reading five channels while processing a pair, and its outputs are
not supposed to equal the physical-pair outputs.

The main comparison gives both programs CPUs `[0,2,4,6]`, a 400% CPU quota,
6 GiB maximum memory, no swap and the same 78°C stop threshold. These are
four physical performance cores, not four SMT siblings. OpenBLAS/OpenMP each
use one thread. No host frequency/fan settings, installed configuration or
services changed. Normal desktop/background activity remained present; this
is not an exclusive-host or theoretical peak-performance measurement.

Each standard case has three alternating-order runs of 12 CPIs. Values are
the **median of the three last-nine-frame means**, excluding three startup/
qualification frames consistently. That yields 27 measured warm frames per
standard variant, not independent recordings. Reference analysis interval is
50 CPIs, so these short runs do not sample a subsequent reference-weight update.
Read/initialization/validation time is separate from instrumented DSP time.

## Five-channel CPU scheduling: four-core budget

`W` is concurrent surveillance workers; `F` is FFT threads per worker.

| Workload | W / F | Median ms/CPI | Run-mean range | Warm frames over 200 ms |
| --- | --- | ---: | ---: | ---: |
| Regular blah2, physical pair, CPU | 1 / 4 | 237.00 | 227.75–257.18 | 27/27 |
| VectorWarp, physical pair, CPU | 1 / 4 | 225.27 | 206.83–248.94 | 25/27 |
| VectorWarp, five-channel CPU | 1 / 1 | 1074.93 | 1059.83–1082.82 | 27/27 |
| VectorWarp, five-channel CPU | 1 / 4 | 893.81 | 876.74–915.46 | 27/27 |
| VectorWarp, five-channel CPU | 4 / 1 | 568.03 | 553.11–584.58 | 27/27 |
| VectorWarp, five-channel AUTO | 4 / 1 | 581.62 | 577.98–599.71 | 27/27 |
| VectorWarp, five-channel explicit GPU | 4 / 1 | 594.44 | 588.48–598.60 | 27/27 |

Changing only channel workers from one to four, keeping one FFT thread per
worker and the same allowed CPU budget, reduced five-channel time by **47.2%**
(1.89× processing throughput). Four workers with one FFT thread also used
**36.4% less time** than one worker with four FFT threads. This is an observed
scheduling benefit within VectorWarp, not a 47% speedup over regular blah2.

Five-channel parallel CPU still took **2.40×** the pair's time. Explicit GPU
was slower than parallel CPU in this profile. AUTO accepted five GPU frames
per 12-frame run and then fell back to CPU; it must be labeled mixed-backend,
not all-GPU. Forced GPU accepted nine GPU frames after three CPU qualification
frames. Five-channel saved outputs matched the same-profile CPU baseline
exactly across the worker/FFT/AUTO/GPU variations.

In the four-worker CPU case, median warm stage means were approximately
364 ms clutter filtering, 89 ms ambiguity processing and 26 ms reference
synthesis. Clutter therefore dominates the remaining workload. FFT threading
is not interchangeable with channel threading: one worker/four FFT threads
took about 399 ms in ambiguity processing, versus 89 ms for four workers/one
FFT thread. One fixed FFT-team size is not optimal for every stage.

## Six-performance-core check

A separate three-repeat comparison gave **both** programs all six physical
P-cores (`[0,2,4,6,8,10]`, 600% CPU quota), retaining the same memory/thermal
limits, profile and warm-frame method. This allows all five channel workers
to run together. Upstream retains its original four FFT threads; VectorWarp
uses five channel workers with one FFT thread each. Equal allowed CPU capacity
does not mean identical thread layout or number of processed channels.

| Workload, ±800 Hz | Median ms/CPI | Run-mean range | Warm frames over 200 ms |
| --- | ---: | ---: | ---: |
| Regular blah2 physical pair, CPU | 210.32 | 203.54–215.81 | 21/27 |
| VectorWarp five-channel CPU, W5/F1 | 376.32 | 356.26–377.57 | 27/27 |
| VectorWarp five-channel GPU, W5/F1 | 414.22 | 406.74–419.30 | 27/27 |

Five-channel CPU now costs **1.79×** the upstream pair time—not five times,
but still **not equal or faster**. Both programs had the same six-core budget.
Do not attribute the difference from the four-core table solely to a code
optimization: resource availability and worker count changed. This also does
not establish real-time 200 ms five-channel operation. CPU/GPU saved outputs
matched the same five-channel CPU baseline exactly.

## Wider Doppler: support versus real-time speed

The following are single 12-frame physical-pair runs, with one worker/four
FFT threads on the same four-core budget. All completed modes missed all
nine warm 200 ms deadlines.

| Requested Doppler window | Upstream CPU ms/CPI | VectorWarp CPU ms/CPI | VectorWarp GPU ms/CPI |
| --- | ---: | ---: | ---: |
| ±2400 Hz | 328.60 | 341.37 | 257.41 |
| ±2500 Hz | Unsafe buffer size; not executed | 349.93 | 253.38 |
| ±4000 Hz | Unsafe buffer size; not executed | 477.10 | 332.47 |

The earlier performance sweep reached ±2400 Hz at this CPI, just below the
tested failure bracket. The limit is **configuration-dependent**, not a
universal maximum Doppler frequency:

| Window | Doppler bins needed | Upstream Doppler buffer capacity | Correlation samples |
| --- | ---: | ---: | ---: |
| ±2400 Hz | 961 | 1000 | 499 |
| ±2500 Hz | 1001 | 960 | 479 |
| ±4000 Hz | 1601 | 600 | 299 |

Upstream sizes the Doppler array using the range-FFT length, then indexes it
using the Doppler-bin count. That is an out-of-bounds-access defect, not a
CPU speed limitation. VectorWarp sizes the array for the actual Doppler count.
The benchmark's source-derived preflight refused unsafe upstream execution;
we did not deliberately corrupt memory to collect a meaningless timing.
The original upstream application is not being credited with that preflight.

At ±2500 and ±4000 Hz, VectorWarp CPU/GPU complex maps passed comparison and
saved detection/track JSON matched exactly for all 12 frames. All requested
delay bins fit the linear-correlation lag support. This demonstrates support
for the tested configurations, **not real-time performance or unlimited span**.

The full five-channel ±2500-Hz workload also completed: CPU **672.34 ms/CPI**,
GPU **762.54 ms/CPI**, both 9/9 warm deadline misses. Saved outputs matched.

### Excluded extreme profile and remaining validation issue

The ±6000-Hz / 256-delay-bin stress case completed numerically at 645.59 ms CPU
and 445.76 ms GPU, both 9/9 warm deadline misses, but **is excluded from usable
coverage claims**. It leaves only 199 samples per correlation block while
requesting positive delays through 245. The existing extraction can relabel
negative circular-correlation lags as positive delay bins.

A bounded FFTW impulse diagnostic reproduced the problem: reference impulse
at sample 170 and surveillance impulse at 15 have true delay **−155**. With
199-sample blocks / a 400-point range FFT, the display extracts the correlation
peak at index 245 and labels it **+245**. The ±4000-Hz geometry uses 299-sample
blocks / a 600-point FFT and correctly leaves this out-of-window peak out of
the requested display (residual below 1e-12).

The ±6000-Hz CPU/GPU maps passed the numerical tolerance, but nine saved frames
differed in SNR values (700 values, maximum difference 1.05 in the legacy
display scale); delay/Doppler positions and tracks matched. Numerical
agreement alone does not establish physically valid coverage. This also exposes
a **remaining settings-validation gap**: allocated FFT length alone is not a
sufficient delay-window check. Reject unsupported signed lags or correct the
extraction before accepting such combinations. No production DSP/validation
change was made during this measurement campaign. Reducing the delay window
would be a different profile, not evidence for the one tested here.

## Actual processor CPI timing, not just the benchmark harness

Four separate native runs replayed 20 real CPIs at sample-rate pacing using
the exact `e17a017` CI package, unchanged through `813618b` in numerical DSP.
They used the same four-core budget and full requested channel configurations.
The last 17 frames produce these actual processor `cpi` timing statistics:

| Native workload | CPU mean / p95 ms | GPU mean / p95 ms | Warm deadline misses |
| --- | ---: | ---: | --- |
| Five-channel array, ±800 Hz, W4/F1 | 539.34 / 572.20 | 592.19 / 657.26 | 17/17 in both modes |
| Physical pair, ±4000 Hz, W1/F4 | 501.06 / 552.82 | 368.58 / 382.85 | 17/17 in both modes |

Every run completed cleanly with 20 timing messages, identical input accounting
(20 CPIs plus 1024 trailing samples), CPU20 or CPU3/GPU17 accepted frames and
the pinned RTX device. These are the application's processing/output timings,
not merely the time to read a recording. Timing boundaries and excluded work
are documented in the [earlier native check](PER_CPI_BENCHMARK_20260910.md#actual-processor-cpi-timing-stream).
Paced replay blocks under backpressure; it does not establish live RF or
drop-free acquisition. Neither native profile kept up with a 200 ms stream.

## Data, limitations and next optimization targets

The [evidence capsule](benchmarks/20260910/array-capacity.json) contains all run
summaries, exact profiles, manifests, source/binary/input hashes, unsupported
cases and native receipts. [Per-frame timings](benchmarks/20260910/array-per-cpi.csv)
include cold/qualification frames and the excluded extreme case. Full commands,
thermal traces and saved detector/tracker outputs remain at
`/var/tmp/vectorwarp-array-capacity-20260910.LkeiSF`; golden maps and raw IQ
remain on the NVIDIA host. The native helper hash is
`5b046433746b8e963a259488aeb2a28e1c492a5f67426b6db65844d36be629cc`.

Upstream detection/SNR/tracker differences remain those documented in the
earlier campaign; these are not identical-output upstream speedup claims.
No new Strix or Pi workload ran. Strix's thermal-stop restriction remains;
the Pi's earlier pair workloads already missed their budgets. Current tests
do not establish equal five-/two-channel time or a universal CPU/GPU gain.

Measured bottlenecks suggest these **untested** next targets: reuse common
reference FFT/covariance work across surveillance filters, test stage-specific
FFT thread allocation, and reduce repeated channel-buffer copying. Persistent
workers could reduce launch/barrier costs, but their benefit has not been
isolated. None of these ideas is credited with a speedup until measured.
