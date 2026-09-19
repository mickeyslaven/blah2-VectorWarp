# macOS processing latency investigation

The September 18, 2026 investigation uses a directly connected five-channel
Kraken on an 8 GiB Apple M2. The operational workload remains 527 MHz,
2.4 MS/s, 200 ms CPI, delay bins −10 through 245, Doppler ±800 Hz,
five surveillance channels, array-eigenbeam reference, clutter cancellation,
detection and tracking. The receiver and transmitter sites are private and are
retained outside the checkout. No raw RF recording is made by these checks.

## What the original spikes showed

The largest original CPU overruns occurred in clutter filtering. The original
automatic/GPU overrun spent 142.6 ms in reference synthesis. Log inspection
established that these frames were not the periodic reference-weight updates;
FFT plans are also created during initialization, not on these warm frames.

A separate diagnostic build, copied from the installed revision 15 source,
records monotonic elapsed time, thread CPU time, process resource counters,
worker launch delays and Mach task events. Host VM counters are sampled once
per second. Stack samples are collected after the measurement window. The
diagnostic executable and private evidence are not included in the package.

In a three-minute GPU run, the two deadline misses were 322.33 and 273.59 ms.
Both coincided with the two largest one-second decompression intervals:
83,389 and 47,071 pages, with a 16 KiB page size. The latter frame's CPU-only
spectrum stage took 65.83 ms elapsed but only 9.71 ms of thread CPU time.
The larger event also included an additional approximately 1.13-second gap
after processing, so CPI duration alone understates the disruption.

The detailed CPU run reproduced a 513.48 ms frame. Its clutter workers took
about 300 ms elapsed while using approximately 84–91 ms of CPU each. Another
frame took 176.62 ms in reference synthesis with 23.28 ms of thread CPU time.
Thread launch delays were generally below 0.5 ms. These measurements establish
substantial off-CPU stalls associated with host memory activity, rather than a
corresponding increase in radar arithmetic. They do not identify the owner of
every decompressed page or attribute every delayed millisecond to paging.

An unchanged installed revision 15 control used the same external collector.
Its median was 85.47 ms, compared with 84.65 ms for the diagnostic run. This
does not indicate a large steady instrumentation cost; host activity and live
RF differed between runs. The control had no deadline misses in its 599-frame
window. GPU-child counters in the initial diagnostic runs were unavailable and
are not used for attribution. Process-wide counters recorded inside concurrent
workers must not be summed or treated as belonging exclusively to that worker.

The [Linux benchmark](GPU_BENCHMARK_20260911.md) recorded no deadline misses
for the same five-channel workload in 24 measured steady frames on Strix.
Those shorter measurements, different hardware, and different RF samples do
not establish a platform-only latency difference or a universal deadline bound.

## Changes under test

- CPU clutter output overwrites the existing surveillance block, preserving
  the original FP64 subtraction and FFT normalization order. It no longer
  clears and rebuilds each channel's sample deque every CPI.
- Array-reference synthesis reuses caller-owned output storage and caches
  read-only input views. It preserves summation order, normalization,
  covariance smoothing and update cadence. Every output sample is reset before
  accumulation, including on CPIs that retain the previous weights.
- CPU clutter prepares the common reference FFTs, autocorrelation and Cholesky
  factor once per CPI. Each surveillance path reads that immutable preparation
  and owns one reusable scratch array. The five-path workload needs about
  58.7 MiB of raw FFT arrays instead of 329.9 MiB. Plans are destroyed before
  their owned storage. GPU startup qualification still executes its CPU oracle
  and can retain these smaller CPU workspaces for fallback.
- The macOS launcher defaults OpenBLAS and OpenMP to one thread, preserving
  explicit overrides. This matches the profiling environment; it does not
  explain spikes measured with those limits already in place.
- A late detection response updates its marker trace without repainting the
  same radar heatmap. Browser updates remain serialized and follow the CPI.
- The native macOS Kraken sender completes partial nonblocking socket writes
  within a bounded deadline. It handles interruption/backpressure, rejects a
  socket that cannot enter nonblocking mode, and closes failed clients before
  another frame can be broadcast. Socket fixtures exercise complete delivery,
  deadline expiry and peer closure.

The reuse changes remove repeated allocation of six 480,000-sample FP64
complex blocks per CPI, approximately 43.9 MiB of payload storage. They retain
the radar's settings, numerical checks, GPU qualification and CPU fallback.
They do not impose a real-time scheduling guarantee on a shared desktop OS.

The subsequent 15-minute CPU run, before shared-reference preparation, failed:
1,970 of 3,431 wholly measured frames exceeded 200 ms. Minute medians rose from
about 128 ms to over 300 ms; the viewer opened after degradation had begun.
Capture queues can add about 183 MiB as they grow from one to six CPIs, and
extracting a CPI starts copying the complete block once the backlog reaches
two CPIs. This provides a concrete memory/copying amplification mechanism.
Measured queue depths were unavailable in that run, so the exact contribution
remains unquantified.

Capture availability must be assessed separately. That run also had a roughly
70-second periodic recalibration gap and repeated native TCP disconnects. The
old native sender treated a partial nonblocking write as a client failure;
VectorWarp then cleared capture buffers and waited a second before reconnecting.
The periodic check reported drift, whose physical or software cause is still
unresolved. A zero coherence-event counter alone does not establish uninterrupted
capture. Calibration checks must remain enabled during acceptance.

Native numerical tests, synthetic replay, configuration effects and actual
Apple-M2 GPU/fallback checks pass. Live candidate and installed-package
comparisons must be assessed using complete frame timings, frame gaps and
concurrent host VM activity, not a mean alone.

## Corrected candidate measurements

The shared-reference candidate passed 24 native tests, 18 replay cases,
11 configuration cases and three real-M2 GPU replay cases. Forced GPU replay
reports Vulkan execution for ambiguity and clutter, with CPU clutter inactive
after startup qualification. The later prefix-input and construction-failure
corrections preserve the measured exact-CPI arithmetic and passed the native
suite again.

| Measurement | CPU | Automatic/forced GPU |
| --- | ---: | ---: |
| Live, 150 s: median CPI | 118.94 ms | 73.39 ms |
| Live: p95 CPI | 139.90 ms | 82.06 ms |
| Live: maximum CPI | 389.10 ms | 267.02 ms |
| Live: frames over 200 ms | 4 / 749 | 2 / 747 |
| Live: processor reconnects | 0 | 0 |
| Same synthetic input, 40 steady frames: median CPI | 110.28 ms | 75.37 ms |
| Same synthetic input: p95 CPI | 119.36 ms | 85.23 ms |

The synthetic comparison uses identical five-channel input and DSP settings;
GPU median processing time is about 32% lower. It excludes USB ingest and does
not substitute for physical testing. Live CPU measurement had the radar page
open; the viewer closed during the GPU measurement, so the live pair is not a
controlled browser-load comparison. Both live runs maintained approximately
five output frames per second, had no gaps over 400 ms, and cleaned up their
owned processes. Neither short run crosses the five-minute calibration check.

All four residual CPU overruns overlap the four largest host decompression
intervals. The two GPU overruns also overlap unusually large host memory
bursts. Process instruction and CPU totals remain ordinary while runnable
time rises, supporting shared-host contention rather than extra radar work.
This does not identify a particular desktop application. The operating system
reported nominal/fair thermal states. `proc_pid_rusage` time fields require
the host Mach timebase conversion (125/3 ns per tick here); treating these raw
values as nanoseconds understates them. Early diagnostic runnable-time fields
with that conversion error are excluded from attribution.

Acceptance targets efficient application work, stable memory and capture,
correct output, and actual GPU speedup. It does not require a hard deadline
guarantee while unrelated applications compete for the same machine. Final
installed Homebrew checks assess receiver continuity separately from
host-contention outliers.

## Installed revision 16 observations

Both installed Homebrew packages passed their formula tests. Sequential physical
CPU and automatic/GPU observations then ran for six minutes each, after startup
and warmup, with the normal settings and no browser. All twelve sampled maps
were finite and 321×256, and both runs stopped their owned processes.

| Measurement | CPU | Automatic GPU |
| --- | ---: | ---: |
| Wholly measured frames | 1,717 | 1,790 |
| Median CPI | 114.30 ms | 73.44 ms |
| p95 CPI | 241.43 ms | 77.27 ms |
| Maximum CPI | 565.86 ms | 135.96 ms |
| Frames over 200 ms | 308 | 0 |
| Maximum publication gap | 1,687 ms | 1,817 ms |
| Confirmed warm processor reconnects | 3 | 0 |

GPU ambiguity and clutter remained on Vulkan, with CPU clutter inactive after
qualification. Processor footprint minute medians stayed between 251 and
261 MiB; its GPU worker retained approximately 408.5 MiB. This supports efficient
GPU operation for the measured interval, rather than a universal deadline bound.

The CPU result does **not** qualify sustained 200 ms operation on this busy Mac.
Minute CPI medians were approximately 110, 108, 110, 122, 187 and 218 ms.
Instruction counts per ordinary CPI remained essentially stable, while CPU time
increased and cycles per unit of executed CPU time decreased. That supports
reduced host execution capacity; the counters cannot distinguish processor
frequency, core placement or the application responsible. It differs from the
isolated off-CPU stalls in the earlier short runs.

The bounded capture queues subsequently amplified the slowdown: extraction
rose from about 0.07 to 6.74 ms and processor footprint from about 162 to
417 MiB. Queue depth was not directly recorded, so its contribution is inferred
from the extraction path and memory bound, not measured as a separate cause.
Three reconnects occurred in the warm interval and another after it. The native
error message conflated write deadline expiry with other socket errors, so it
does not establish which failure caused each reconnect.

Both runs had a gap near the expected five-minute periodic calibration check.
No drift or full recalibration was reported, but successful/skipped check messages
were suppressed by native noninteractive logging. Elapsed duration alone cannot
prove the check succeeded. Timestamped calibration outcomes and classified
transport failures are being checked in a separate diagnostic candidate; the
calibration thresholds and 25 ms transport budget remain unchanged. The generic
runner's success flag establishes completion, fresh sampled output and cleanup;
it is not acceptance of sustained CPU performance or uninterrupted capture.

## Direct calibration and transport verification

A subsequent six-minute CPU observation used the **same installed revision 16
processor**, with a native candidate that changes diagnostic logging only.
The 25 ms socket deadline and every calibration threshold remained unchanged.
All 1,788 measured frames were below 200 ms: median 108.33 ms, p95 134.19 ms,
maximum 155.23 ms. There were no processor reconnects or warm transport failures.
Six sampled maps were finite and 321×256, and all owned processes stopped.

The only long publication gap was 1.790 seconds. Timestamped native events
place the scheduled calibration check inside that gap: eight snapshots completed
in 1.150 seconds and reported OK, with worst residual lag 0.01921 samples and
phase 0.82997°. Fresh output followed the OK event by 553 ms. This is deliberate
noise-calibration gating, not a processing overrun or full drift recovery.
The sole socket failure was a peer closure during shutdown, after the observation
and stack sample had ended.

Processor footprint had a 169.27 MiB median and 225.52 MiB maximum; minute
medians remained 168–173 MiB and extraction stayed near 0.06–0.07 ms. The final
minute's CPI median rose to 130.56 ms with a decrease in cycles per executed CPU
time, while instruction totals stayed stable. It retained processing headroom
without accumulating the earlier backlog.

This accepts the bounded CPU observation and directly verifies one periodic
check. Logging did not fix the earlier sustained CPU slowdown, and the earlier
reconnects' exact socket failure class was not captured. Those failures remain
part of the evidence. Host capacity varies; neither this repeat nor the GPU
result establishes unlimited endurance or a hard real-time guarantee. The new
diagnostics distinguish deadline expiry, peer closure and other socket errors
on future runs without logging every successful frame.

Homebrew revision 17 installs these native diagnostics. Both formula tests pass.
The application, capture core, receiver modules and GPU binaries are byte-identical
to revision 16. Packaging validation also fixed the app's native executable and
HTML links to retain Homebrew's stable `opt` path, so a companion upgrade is
used instead of remaining pinned to its previous Cellar version. That packaging
change does not alter radar arithmetic or require another physical soak.
