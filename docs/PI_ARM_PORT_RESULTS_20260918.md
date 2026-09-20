# Pi ARM port recorded-IQ campaign results — 2026-09-18

This note records the Pi 4B performance and regression campaign on
`codex/pi-arm-improvements`, based on VectorWarp main `a1fe4cdd647030823e6ae20813717ac0081e09cd`.
The CPU improvement is verified, but the full 500 ms live workload is not
qualified: its processing cost still causes capture FIFO overflow.

The implementation includes blocked clutter correlation and overlap-save FIR,
shorter paired ambiguity transforms with bounded FFT threading, and an exact
folded-spectrum fast path with the original fallback. Capture uses reusable
paired callback storage and guarded SDK-counter/USB opt-ins. TCP JSON framing
is per connection and bounded; queue-drop telemetry includes a final snapshot
through receiver shutdown.

## Provenance and method

The runs used the Pi 4B's Debian 12 aarch64 environment through the ADS-B Zero
relay and the immutable recorded input
`/var/tmp/owl-field-20260914/live20-s16le-iqAB.raw`:
`86753d5e96a6e05ab2654a733e908076fad3e1e0373ee9e79e4303ce828367a3`.
It is 20 seconds of paired, signed-16-bit A-I/A-Q/B-I/B-Q ADC samples at 2
MS/s and 551 MHz. `BenchmarkReader` supplied those original ADC integers as
doubles; neither revision normalised or rescaled the input. Both revisions
used the same reader and JSON instrumentation; baseline DSP remained at the
pinned VectorWarp main revision.

The 500 ms paired profile processed 1,000,000 samples/channel per frame, 411
delay bins, 301 Doppler bins, one worker, four FFT threads, and Hamming-number
FFT-length rounding. Baseline used a 6,750-point range FFT; the candidate's
shortened requested-lag geometry used 3,750 points. Each final repeat is 40
frames with the first eight classified as warm-up, leaving 32 steady frames
per run and 96 pooled steady frames per revision. The harness paced input at
the sample clock; these are DSP/pipeline processing times rather than a claim
of live end-to-end latency.

The runner's CPU, RAM, temperature, and free-disk guards were enabled with a
four-CPU / 4 GiB container budget and a 72 C temperature ceiling. The maximum
recorded temperature across the guarded workloads was 45.764 C. Compact
source evidence is retained under the ignored
`build/pi-port/evidence/campaign-20260918/` directory; it contains only
summaries, frame timing CSVs, comparator output, and GPU-driver provenance.
Every listed completed benchmark guard exited with return code zero.

### Reproducibility manifest

All SHA-256 values below were read from the Pi campaign directory after the
benchmark runs. The candidate DSP source hashes identify the tested build;
later working-tree changes are limited to capture, tests/harness, API/UI work
and are not represented by these benchmark results.

| Artifact | SHA-256 / identity |
| --- | --- |
| `build-baseline/bin/bench-fast` | `5c779d93367b9d4fc955312b8ca5c8929081d21c3ed2978a697a5b72e4952cbf` |
| `build-candidate/bin/bench-fast` | `5b4df1875aec3aba5288936917837761609bfdb0670766ae633dd4fd2cd6bd9e` |
| `profile.json` | `fec1ead47aa8e81f626bcce70513aed79adcb12091cac2b1fa8854552112ea02` |
| `candidate-v2.tar.gz` | `3396a2a473637f64cf33fb62ef1f4a4301c752fc554992c42e017032e6397c53` |
| `candidate/src/process/ambiguity/Ambiguity.cpp` | `6d64ddebf8be9a322c218767c4847fa416460f2cafd4931150f13f6dd4afef65` |
| `candidate/src/process/clutter/WienerHopf.cpp` | `7bf3fa9272251b1b380d1c3348fad58bf380e1e6f232d23a8493946ae412dc47` |
| `candidate/src/process/spectrum/SpectrumAnalyser.cpp` | `7f180983f85b949e86a48d52592941b1224db2c386d834cb94a5d6fe681e9589` |
| `owl-local:integrated-build-deps-20260916` | `sha256:ab29c9968ef993f01296aa33cd5ec3e719a8c01418c116a952316a95441839ff` (local image; no repository digest) |

## 500 ms CPU result

| Revision / run | Steady frames | Mean ms | p50 ms | p95 ms | p99 ms | Max ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline 1 | 32 | 1334.005 | 1326.453 | 1368.901 | 1475.510 | 1522.111 |
| Baseline 2 | 32 | 1347.781 | 1338.501 | 1385.700 | 1476.954 | 1516.180 |
| Baseline 3 | 32 | 1364.878 | 1356.683 | 1396.227 | 1519.523 | 1571.112 |
| **Baseline pooled** | **96** | **1348.888** | **1342.248** | **1389.383** | **1524.561** | **1571.112** |
| Candidate 1 | 32 | 565.100 | 564.784 | 572.810 | 576.353 | 577.594 |
| Candidate 2 | 32 | 562.161 | 560.078 | 566.598 | 618.153 | 641.096 |
| Candidate 3 | 32 | 565.120 | 563.293 | 576.898 | 577.863 | 577.956 |
| **Candidate pooled** | **96** | **564.127** | **562.910** | **575.996** | **581.113** | **641.096** |

The candidate reduces pooled steady processing by 58.18% (2.39x faster).
All 96 pooled frames for both revisions missed the 500 ms processing deadline, so this
is an offline throughput gain and does not establish real-time operation at
this workload. The candidate's isolated-process peak RSS was 136,248–136,352
KiB, versus 259,112–259,128 KiB for the baseline.

Each final baseline/candidate repeat was semantically compared independently:
all three 40-frame comparisons passed with 80,861 numeric values, 129
detections, two tracks, and zero reported JSON difference. The candidate's
complex-map RMS/peak relative differences were 4.419e-11 / 6.005e-10 on all
three repeats. These comparisons use the existing absolute 0.011 and relative
1e-6 JSON tolerances; the small map metrics are separate harness values.

## 200 ms CPU check

The same input, profile and resource settings at 200 ms (400,000
samples/channel, 121 Doppler bins) produced one 40-frame run per revision:

| Revision | Steady frames | Steady mean ms | p95 ms | p99 ms | Max ms | Steady deadline misses |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline | 32 | 573.976 | 616.289 | 703.329 | 734.810 | 32 |
| Candidate | 32 | 246.401 | 250.080 | 250.271 | 250.349 | 32 |

This is a 57.07% reduction (2.33x), but it is one run per revision and every
steady frame still exceeded the 200 ms period. It is therefore a regression
check, not a confidence interval or a live-rate acceptance result. Its
40-frame semantic comparison passed with 80,563 numeric values, 41 detections,
zero tracks, and zero reported JSON difference; the candidate map RMS/peak
relative differences were 1.058e-10 / 2.585e-9.

## Ablation screens

The frozen v1 screens were 40-frame CPU runs under the 500 ms geometry. They
are useful attribution evidence but are older, non-alternating measurements;
they must not be combined with the final repeated campaign as independent
replicates.

| Screen | Steady mean ms | p95 ms | Peak RSS KiB |
| --- | ---: | ---: | ---: |
| Spectrum only | 1303.161 | 1353.986 | 243444 |
| Ambiguity only | 1271.183 | 1313.154 | 259008 |
| Clutter only | 783.295 | 822.323 | 152372 |
| Combined | 632.634 | 659.997 | 136372 |

Each screen's 40-frame semantic comparator passed: 80,861 numeric values,
129 detections, and two published tracks were exactly equal in the emitted
JSON. The comparator tolerances were absolute 0.011 and relative 1e-6; a zero
reported maximum difference means the rounded JSON values were identical, not
that internal floating-point maps are bit-identical. The corresponding maps
were separately qualified by the harness; they are intentionally not retained
in this evidence copy.

## V3D qualification

The GPU runtime identified itself as `Vulkan / V3D 4.2.14.0`; the copied
driver manifest identifies the host Broadcom Vulkan library and its SHA-256.
The 40-frame `auto` run had 37 GPU and three CPU pipeline frames. Clutter
attempted GPU work on three frames but retained CPU results for all 40 after
its acceptance/fallback path. Its 32 steady frames averaged 630.995 ms
(p95 654.627 ms), slower than the 564.127 ms CPU candidate pool. The semantic
comparison against the 40-frame baseline passed exactly (80,861 numeric
values, 129 detections, two tracks).

Forced V3D is not equivalent to a production GPU result. It completed only 16
500 ms frames: eight GPU and eight CPU pipeline frames, with GPU clutter
executed on all 16 and CPU clutter retained on eight. Its steady mean was
1012.790 ms (eight frames), p95 1016.117 ms, and all 16 frames missed the
deadline. The local comparator explicitly compared the first 16 baseline
JSON frames with the forced output and passed: 32,317 numeric values, 47
detections, zero tracks, zero maximum numeric difference at the same
tolerances. Its map RMS/peak relative values were 6.470e-6 / 5.408e-5, versus
4.269e-7 / 3.510e-7 for auto; neither result justifies enabling V3D by
default.

## What this does and does not accept

The evidence supports the CPU candidate's recorded-IQ performance and emitted
JSON semantic equivalence for the measured paired profile. It also supports
keeping GPU work guarded and CPU-fallback-capable: auto V3D is slower here and
forced V3D has limited, non-production coverage.

## Application regression checks

The complete Pi application built with GCC 12.2, the existing SDK 3.15, and
GPU support enabled. All 25 CTests passed, including receiver-module isolation,
callback failure handling, FIFO accounting, GPU qualification/fallback, the
DSP suites, and multichannel processing. All 24 Linux SDRplay local source-kit
tests passed. The five distribution workflow tests passed on macOS; their Pi
build-container attempt failed because that image has no Node.js runtime.

All 18 full-processor offline replay cases passed on the Pi: two-channel
RSPduo/USRP/HackRF recordings, Kraken recordings with 2–8 channels, looping,
missing/mismatched input, invalid configuration, and 6 MS/s processing with
clutter enabled. The replay harness also verifies the final stopped-capture
status and zero replay FIFO overflow.

The complete API suite and health/insights smoke checks passed locally.
Independent DSP oracles cover signed and boundary lags, nonzero Doppler
centers, odd/nondivisible spectra, raw ADC-scale clutter, short/partial blocks,
and singular inputs. Clutter ASan/UBSan checks passed; leak sanitizer was not
supported by the macOS runtime. All five benchmark CTests passed on the Pi,
including the pinned upstream contract and reader/comparator checks. The live harness passed five test
groups containing 31 fake-processor scenarios, including incomplete telemetry,
post-final-CPI drops/faults, invalid status, short observation, and shutdown.

## Live RSPduo limits

The complete application was tested at 2 MS/s, 551 MHz, 500 ms CPI, delays
-10 through 400, Doppler ±300 Hz, 410 clutter taps, detection/tracking enabled,
one worker and four FFT threads. The configured two-CPI capture FIFO holds
2,000,000 samples per channel. Temporary loopback HTTP/TCP peers drained every
output stream; the installed API/web/host containers were not replaced.

Both the default isochronous transport and the optional bulk transport failed
the requested 60-second qualification early. The harness detected FIFO overflow,
stopped each process cleanly, and received the final counters: 619,284 dropped
samples per channel with isochronous USB, and 566,868 with bulk. Five timing
frames were observed in each run: 650.69–726.35 ms per CPI with isochronous
USB and 639.34–715.13 ms with bulk.
Neither run reported a tuner-pairing fault. These are failed live acceptance
results, not zero-loss or real-time claims. There was no live baseline A/B run;
the recorded baseline also exceeds the processing deadline.

A separate diagnostic binary timed 20 recorded frames (12 after warm-up).
Mean clutter phases were: input copy/rotation 12.729 ms, correlation 153.129 ms,
factorization/solve 32.883 ms, FIR filtering 97.290 ms, output replacement
19.980 ms, total 316.013 ms. This supports further work on correlation/filter
scheduling and queue ownership; changing USB transport does not remove the
remaining compute cost. The diagnostic patch was not added to production code.

A clutter-disabled capture check is recorded separately below; it cannot
qualify the full clutter-enabled workload. The guarded Toeplitz
solver remains deferred; earlier real-IQ evidence showed 263/263 fast-path
declines and added work. Paired whole-CPI queues, new correlation threads, and
the PR's in-process split V3D backend were not transplanted.


## Capture stability check and final host state

A separate three-minute run retained the same capture rate, frame size, radar
geometry, detector/tracker, and thread budget, but **disabled clutter**. It used
isochronous USB and the guarded `VECTORWARP_RSPDUO_COUNTER_SCALE=3` mode.
The accepted SDK receipt matched the requested configuration. Over 180.002
measured seconds it produced 359 measured timing frames (361 including startup
and shutdown), p50 346.06 ms, p95 380.644 ms, maximum 475.87 ms. Both final FIFO
overflow counters were zero; no capture faults were reported, and shutdown
completed normally. Final backlog was 690,560 samples per channel. This checks
capture continuity/cadence under the reduced compute load, not the full clutter
workload, live counter rollover, or RF/channel coherence.

The first attempt at that check was invalidated by the existing root cron job
`/opt/blah2/script/blah2_rspduo_restart.bash`: it killed/restarted the SDK service
at 13:05:01 UTC while checking the installed API, which was not receiving this
isolated test's output. That run produced only 126 measured frames and needed
a forced stop; its failed verdict is retained. The successful repeat ran from
13:10:28 UTC between scheduled restarts. No maintenance settings were changed.

After testing, SSH, NetworkManager, Tailscale, Docker and SDRplay were active;
`vcgencmd get_throttled` returned `0x0`. The original API/web/host containers
remained running, all temporary test containers were gone, and the Zero relay
was still usable. No host OS, boot, card, or installed application changes were
made. Live/CTest/replay evidence and final service receipts are retained under
`build/pi-port/evidence/acceptance-20260918/` (ignored, including private device
receipts). Licensed SDK files and raw IQ were not copied into the repository.

Final application SHA-256:
`6632008f3cd6799b7f7ba1a87e35f668d214d580363bb2664985e37f0d93e71b`.
RSPduo module SHA-256:
`4c6e6e002cb841cf9eb4744f53695ec638af7d70d510478f68f4c1ac6f3ef7a9`.
The benchmark binary's hash remained unchanged after the regression checks.
