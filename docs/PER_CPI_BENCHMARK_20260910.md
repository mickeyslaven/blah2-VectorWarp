# Current-source per-CPI benchmark — 2026-09-10

On the tested NVIDIA host, VectorWarp's GPU reduced the standard profile's
instrumented pipeline from **223.91 to 163.38 ms/CPI** versus its own CPU path.
That is the supported GPU execution-path comparison. CPU-only changes were near
parity or slower, and the reduced-load Pi profiles did **not** meet real-time
budgets. These are recorded-IQ measurements, not live RF acceptance.

## Scope and reproducibility

Results come from `nvidia-confirm`, `nvidia-pilot` and `pi-pilot` under the
retained campaign directory `/var/tmp/vectorwarp-capacity-20260910.i3klJn`.
Each comparison uses upstream and VectorWarp on the **same hardware**, with
identical selected samples and settings. No cross-device speed comparison is
made. The input contains five recorded channels; these tests process physical
reference channel 0 and surveillance channel 1 only, without array synthesis.

All profiles retain the actual **2.4 MS/s, 527 MHz** recording metadata and zero
overlap. NVIDIA uses delay −10…245 (256 bins), clutter −10…200 (210 taps),
CFAR guard/train 2/6, PFA 0.0001, minimum delay/Doppler 1/5, centroid 6 and
tracker M/N 12/25, deletion 40, maximum acceleration 0. Profiles use one
surveillance worker, four FFT threads and Hamming-number FFT-length rounding;
the latter is not an amplitude taper. Exact profile JSON is retained with runs.

The run owner's host setup was Fedora 43, Intel Core i7-12650H, RTX 4050 Laptop,
NVIDIA driver 580.173.02, four P-core CPUs and a 6-GiB memory budget. Manifests
confirm affinity `[0,2,4,6]`, four FFT threads and device `4318:10401:0`.
OpenBLAS/OpenMP were each limited to one thread. These are results under that
budget, not maximum theoretical hardware performance. The Pi comparison used
Fedora 44 on Raspberry Pi 4, affinity `[0,1,2,3]` and four FFT threads.

Pinned provenance from each run's manifest:

| Artifact | Commit or SHA-256 |
| --- | --- |
| VectorWarp source base | `813618b81bf73283f1cf3e2c1aefda104ee2d0f8` |
| Pristine upstream DSP | `c821bee3f0d27cf20c8447f3d908ef722905a4de` |
| Instrumentation `Pipeline.cpp` | `28abc1c21e0a9fe0e43e9d7630da436abdaf4cdabb8f2a6811efbc53681148c6` |
| Instrumented source archive | `cb140cee23e09d86aa0719377d5c919223450c2b40e7c495278b538adf5b7b95` |
| NVIDIA upstream binary | `ef0851b02cfd1c211a00be1048b5a1c12d3f89ece23950d102514956d82e5c35` |
| NVIDIA VectorWarp binary | `8cca7e7e8375ebcab7ae4b8b73d870bddee742902fce78adcbdc2f322a92c79a` |
| Pi upstream binary | `65493c037dd7db7cd093e57c790d7fefb5d4b46e174480ba7bf31d67085c4d59` |
| Pi VectorWarp binary | `4459454b12db07f8ab8bde51430c45bd8eca6a84ebdbf423d9d279866029b60e` |
| NVIDIA input | `a10d35d25913ea5937c926a4de86ab1ffc30e7e0a4a81dbc34ba93d997435da5` |
| Pi input | `4188cd03768cf356b31e26759c1a75ec83ffc3c83ecfdc16bbb3e9bfc242d744` |

The manifests also retain GPU module/worker hashes. The benchmark links the
pinned DSP classes, not the installed application's main loop. Its monotonic
pipeline timing includes extraction/reference preparation, spectrum, clutter,
ambiguity, metrics/fusion, detection, tracking and JSON. Reading, initialization,
correctness validation and network/live acquisition are excluded and must not
be confused with processing latency or end-to-end replay wall time.

## NVIDIA: three repeated runs

Each run processes 20 CPIs. The first three are excluded from the warm statistic
for every mode; GPU modes use them for CPU/GPU qualification. Each table cell
is the **median of three 17-frame warm means**, in ms/CPI. Deadline counts pool
the resulting **51 warm frames per mode/profile**. Run order alternates.

| CPI / Doppler span | Upstream CPU | VectorWarp CPU | AUTO | Explicit GPU |
| --- | ---: | ---: | ---: | ---: |
| 200 ms / ±800 Hz | 228.11 | 223.91 | 164.72 | 163.38 |
| 200 ms / ±1200 Hz | 247.08 | 244.37 | 187.06 | 185.62 |
| 50 ms / ±1600 Hz | 79.26 | 85.73 | 54.09 | 58.65 |

| Warm frames exceeding CPI budget | Upstream CPU | VectorWarp CPU | AUTO | Explicit GPU |
| --- | ---: | ---: | ---: | ---: |
| 200 ms / ±800 Hz | 50/51 | 44/51 | 1/51 | 0/51 |
| 200 ms / ±1200 Hz | 51/51 | 51/51 | 14/51 | 3/51 |
| 50 ms / ±1600 Hz | 51/51 | 51/51 | 48/51 | 49/51 |

Every AUTO and explicit-GPU run accepted 17 warm Vulkan frames. Standard-profile
explicit-GPU warm means ranged 162.64–165.12 ms; upstream ranged
226.28–230.25 ms. Full per-frame and startup data remain in the artifacts.
Zero misses in 51 observed warm GPU frames is encouraging but is not a
sustained live-acquisition guarantee. The larger span still missed deadlines;
the 50-ms profile exceeded its budget in nearly every frame despite acceleration.

The measured mechanism is concentrated in ambiguity processing: standard-profile
median warm stage means were **87.95 ms on current CPU versus 24.30 ms on GPU**,
including GPU transfer/import work. Clutter remained approximately **77–78 ms**;
it was not GPU-accelerated. Single-path fusion cost was included. CPU changes
cannot be advertised as a universal speedup: the short-profile current CPU was
slower than upstream, and small differences elsewhere do not isolate one patch.

The earlier one-run pilot also tested heavier cases. At 200 ms/±2400 Hz,
upstream/current CPU/AUTO/GPU took 366.34/340.23/250.35/257.10 ms per warm CPI;
all missed all 17 warm deadlines. At 500 ms/±1200 Hz they took
613.46/608.51/513.04/544.38 ms; AUTO missed 14/17 and the other modes 17/17.
These single-run results are not replicated claims or evidence of unlimited
Doppler coverage at real-time cadence.

## Pi: reduced load still failed its budget

Both CPU implementations used delay −4…27 (32 bins), Doppler ±400 Hz, clutter
−2…14 (16 taps), and minimum detection delay 5. Other controls stayed matched.
Each was a single seven-frame pilot, with four warm frames after the same
three-frame exclusion. No Pi GPU performance is claimed here.

| CPI | Upstream CPU ms/CPI | VectorWarp CPU ms/CPI | Warm deadline misses, each |
| --- | ---: | ---: | ---: |
| 200 ms | 1889.77 | 1870.28 | 4/4 |
| 100 ms | 680.63 | 674.03 | 4/4 |

Current CPU clutter alone took 1660.08 ms and 569.34 ms respectively. Reducing
the tap count therefore did not deliver the expected savings. The convolution
FFT length is `N + taps + 1`: these settings produce prime lengths **480017**
and **240017**. Their difficult FFT factorization is a plausible explanation
for the disproportionate clutter cost, supported by source dimensions and
stage timing, **not established by FFT-library/driver profiling**. Spectrum,
extraction and ambiguity also retain substantial full-sample work.

Reduced range/Doppler and fewer clutter taps sacrifice coverage and delayed
multipath cancellation. They do not establish equivalent radar capability.
The recording was not relabeled to a lower sample rate, and no live radar
configuration was changed. Neither tested Pi setting was real-time capable.

## Correctness: GPU agreement, not universal upstream equivalence

Across pilot and confirmation, **480 saved-frame comparisons** in 24
current-CPU-to-AUTO/GPU run pairs matched exactly, including all detection and
track JSON and IDs. These reuse finite recorded windows; they are not 480
independent real-world exposures. GPU complex-map errors remained below 1e-4;
the observed maximum relative RMS/peak errors were approximately
1.21e-6/1.51e-6. Golden maps use stored `complex<float>` precision, not a claim
of exact FP64 equality. Complex maps were compared before magnitude fusion.

Against upstream, match detections by coordinates before comparing fields;
extra detections otherwise shift list indices and create misleading large
"position differences." Every shared detection had exactly identical saved
delay/Doppler coordinates: **zero 0.01-unit rounding discrepancies and no
upstream detections removed**. Confirmation results per 20-frame run were:

| Profile | Additional edge detections | Shared detections with higher SNR | Largest increase | Frames with substantive tracker differences |
| --- | ---: | ---: | ---: | ---: |
| Standard 200 ms | 3 in 2 frames | 69/170 | 0.95 | 10 |
| Mid-wide 200 ms | 4 in 4 frames | 107/234 | 0.96 | 7 |
| Fast 50 ms | 1 in 1 frame | 29/58 | 0.87 | 4 |

These results repeat across all three confirmation runs. Extra detections occur
at delay 245 or the outer Doppler boundary. Upstream interpolation explicitly
drops those edge detections; current code preserves finite boundary detections.
Shared-detection SNR changes are increases only and correspond to retaining the
delay-axis peak instead of overwriting it during Doppler interpolation. These
are legacy display-scale units, not ordinary physical SNR dB or demonstrated
sensitivity gains. In the wide/heavy pilots, respectively, 2/1 edge detections
were added; 212/465 and 291/645 shared SNRs increased, by at most 1.74/1.42.

Tracking differences are not random-ID artifacts. Upstream's association gate
uses zero-initialized predicted coordinates instead of the computed prediction;
current code uses the prediction and retains successful associations. Current
tracking made one association in each confirmation profile, versus none
upstream. Every frame's total-count difference exactly equals cumulative edge
additions minus current associations. Wide/heavy pilots made three/four
associations. **No ACTIVE or COASTING tracks occurred in any of these outputs.**
Do not claim established-track equivalence or independently validated target
tracking from these short windows.

## Actual processor: CPI timing stream

This separate check runs the **real packaged VectorWarp processor**, not the
instrumented DSP executable. It reads a whole-packet, byte-identical prefix of
the NVIDIA input above with sample-rate pacing enabled: 20 complete 200-ms CPIs
and 1024 trailing samples, with the same ±800-Hz profile, physical channels 0/1,
four-core CPU budget, FFT threads and non-driver DSP libraries. Loopback peers
drain its normal TCP outputs and collect its timing/status messages. No
receiver, radar service or installed configuration was changed.

| Native `cpi` timing, ms | VectorWarp CPU | VectorWarp explicit GPU |
| --- | ---: | ---: |
| Steady mean, last 17 frames | 239.46 | 180.31 |
| Steady p95 | 273.59 | 197.21 |
| Steady maximum | 310.09 | 212.23 |
| Steady frames exceeding 200 ms | 17/17 | 1/17 |
| First frame, including qualification work | 316.31 | 367.65 |
| Final extraction-schedule drift | +850 ms | +1 ms |

This is one finite run per mode, not a three-repeat native-application result.
GPU accepted 17 Vulkan frames after three CPU-owned qualification frames; CPU
accepted all 20 on CPU. Both reported clean replay completion and accounted
for the same input samples. The native GPU mean was 24.7% below its own CPU
path. **There is no full-application upstream measurement in this table.**

The native `cpi` field times extraction through spectrum, clutter, ambiguity,
metrics/fusion, detection, tracking, serialization and TCP outputs. It excludes
input-file ingestion/replay waits, initialization, the final timestamp send,
status polling and the API/browser. Thus it is the application's processing
timing, not launch-to-exit time or an end-to-end live latency measurement.

GPU accumulated a maximum +278-ms extraction-schedule drift during startup
and recovered to +1 ms by the last frame. Its timing-frame timestamp interval
p95 was 272.6 ms; one steady frame took 212.23 ms. Do not claim continuously
backlog-free processing. CPU ended +850 ms behind. Launch-to-complete was
7.69 s CPU / 7.36 s GPU for this four-second input, including initialization
and completion reporting. The replay reader blocks under backpressure;
live acquisition can instead discard old samples. **This is sample-rate-paced
full-processor replay, not live RF or drop-free acquisition verification.**

The package is the Ubuntu 22.04 x86-64 CI artifact from successful run
[`34539847358`](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/34539847358),
source `e17a017e7f536e02ba560b64320848ddefd21252`. Changes through the benchmark
base `813618b` add diagnostics/comments, not DSP arithmetic; the packaged
binary itself is **not** labeled as an `813618b` build. It ran extracted,
without installation, on the same Fedora 43 NVIDIA host using the benchmark's
non-driver libraries and the native host Vulkan driver. This does not certify
installing Ubuntu packages on Fedora.

| Native artifact | SHA-256 |
| --- | --- |
| Official DEB | `91b03395bcb37897fdd224db1cca5fcb38947e519fbd77aaa147d9ded8c58715` |
| Processor binary | `10cc2ce0dee34803b2a658caed9476d20b7088728d58fbeec61862f05f6bda40` |
| Finite real-IQ prefix | `89bfe1c338995167946b7c367b80480143b60a515d9eabde39380bcc48e03a5d` |
| Measurement helper | `d64013095c8d6de70ada777041e2801c7c8ed73d38e49234e4a7e1b7f99dd6d9` |

## Data and repeatability

The checked-in [run summaries and profiles](benchmarks/20260910/results.json),
[individual instrumented CPI timings](benchmarks/20260910/per-cpi.csv) and
[native processor receipts](benchmarks/20260910/native-timing.json) retain the
numbers behind these tables, including cold/qualification frames. Summaries
include the original manifests and source/binary/input hashes. Native receipts
retain all 20 timing messages, generated configurations and completion status.
These are two different timing definitions; do not pool them.

The [benchmark harness](../bench/README.md) documents matched builds and the
geometry preflight. Full local logs, commands, per-frame detection/track JSON,
thermal traces and native receipts are retained under
`/var/tmp/vectorwarp-capacity-20260910.i3klJn`. CPU golden maps and the raw IQ
remain on the respective hosts; raw IQ is not included in this repository.
The finite native helper and exact-source support kit are retained under
`/tmp/vectorwarp-processor-replay-perf.iQGTCI` with their command handoff.
No Strix DSP benchmark was retried after its earlier thermal stop. This
campaign's NVIDIA confirmation peaked at 49°C and its Pi pilot at 45.764°C;
all guarded runs completed without a thermal retry.

## Untested optimization candidates

Convolution FFT padding to a suitable smooth length at least as long as the
required linear convolution could avoid prime-size plans, but requires correct
zero-padding, normalization and equivalence tests; it was **not implemented or
benchmarked here**. Further copy/serialization work should be guided by measured
stage cost. Genuine common anti-alias-filtered decimation would change bandwidth
and range resolution and needs separate input/filter/detector acceptance; merely
changing the sample-rate field is invalid. None of these ideas earns a measured
speedup or a Pi real-time claim.
