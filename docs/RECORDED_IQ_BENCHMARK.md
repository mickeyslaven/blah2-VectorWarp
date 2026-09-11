# Recorded-IQ benchmark report — completed 2026-09-10 campaign

Historical evidence only; not the current performance comparison. Use the
[fixed-range results](GPU_BENCHMARK_20260911.md) for current speed claims.

## Scope and evidence

This report covers completed runs only. Campaign evidence includes the raw IQ,
frozen source archives, binary hashes, per-frame measurements and
`summary-final.{json,csv}`. These large artifacts are retained separately from
the source repository. Final summarization ran at 50% CPU and an 8-GiB memory
limit after the timed benchmarks had finished.

The input is the independently verified calibrated MCHQ recording:

| Property | Value |
| --- | --- |
| SHA-256 | `a10d35d25913ea5937c926a4de86ab1ffc30e7e0a4a81dbc34ba93d997435da5` |
| Bytes / packets | 2,881,409,048 / 17,579 |
| Channels / tuning | five / 527 MHz |
| Samples per channel / duration | 288,014,336 / 120.005973 s |
| Verification receipt | `kraken-120s.mchq.verified.json` |

It is a signal-processing benchmark: validated MCHQ input through reference,
spectrum, clutter, ambiguity, detection, tracking, and JSON stages. Capture,
network and browser throughput are excluded. Read and validation timings are
reported separately; pipeline timings below exclude them. Every completed run
has 600 CPI frames (0.2 s) and 288,000,000 consumed samples/channel, with the
14,336-sample tail reported separately.

The comparison is pinned to frozen archives in the campaign root, not the
working tree after later recording/UI/tracker changes:

| Artifact | SHA-256 |
| --- | --- |
| Fork processing source, `fork-processing-source.tar.gz` | `d120bb103f40ba0fb7f307233d8b905b865e36e547545d56aea7c630b4b75ac6` |
| Pristine upstream archive, `upstream-c821bee.tar.gz` (upstream `c821bee3f0d27cf20c8447f3d908ef722905a4de`) | `f1e7354d88ecddb94ba4ab8bf23596334a12b6ef3b252be09ec77b453d9ec4b1` |

Package binary hashes are in `package-sha256.json`. These results therefore do
not assert that all later current-tree fixes or defects were exercised.
In particular, these runs predate the sustained-cost AUTO fallback and the
tracking/spectrum math repairs listed in [Upstream fixes](UPSTREAM_FIXES.md).

## NVIDIA RTX 4050 Laptop — completed matrix

Host `fedora` is the RTX 4050 Laptop system (`4318:10401:0`). Values are the
median of three full 120-second recordings; ranges are min–max pipeline time.
`auto` selected the GPU for 597 of 600 frames in every completed GPU run.

### Same-physical-pair upstream comparison

This is the only upstream-equivalent comparison. It uses one physical reference
and one physical surveillance channel, so it must not be conflated with the
five-channel array profile.

| Variant | Pipeline s, median [range] | Speed vs upstream CPU | Full replay wall s, median | Correctness |
| --- | ---: | ---: | ---: | --- |
| Upstream CPU | 128.740 [128.692–130.761] | 1.000× | 141.520 | baseline |
| VectorWarp CPU | 129.389 [127.819–131.050] | 0.995× | 142.293 | exact outputs |
| VectorWarp AUTO | 98.668 [98.622–102.561] | 1.305× | 113.598 | GPU 597/600; detections/tracks match |
| VectorWarp GPU | 100.687 [100.393–102.177] | 1.279× | 115.173 | GPU 597/600; detections/tracks match |

For all nine pair GPU/CPU fork runs and all three upstream runs, the summary
reports zero detection disagreement frames, zero track disagreement frames and
zero rounding-only frames. Pair GPU map error was bounded at RMS relative
`1.2587e-6`, peak relative `2.4523e-6`.

Median per-CPI stage time (ms) for the same pair:

| Variant | Extract | Spectrum | Clutter | Ambiguity | Detection | Tracker | JSON | Read |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Upstream CPU | 5.220 | 9.974 | 76.814 | 83.714 | 16.682 | 0.113 | 22.317 | 18.776 |
| VectorWarp CPU | 5.953 | 9.979 | 77.880 | 86.161 | 14.679 | 0.110 | 20.860 | 18.873 |
| VectorWarp AUTO | 6.100 | 9.636 | 75.555 | 37.612 | 15.046 | 0.119 | 20.991 | 19.240 |
| VectorWarp GPU | 6.168 | 9.557 | 76.512 | 38.527 | 15.899 | 0.117 | 21.352 | 19.280 |

### Five-channel array profile — fork scaling only

The array profile uses the five-channel recording and synthesized/all-channel
reference configuration. No upstream five-channel equivalent was run, so it is
not a fork-vs-upstream speed claim.

| Variant | Pipeline s, median [range] | Relative to array CPU | GPU frames |
| --- | ---: | ---: | ---: |
| VectorWarp CPU | 216.134 [214.200–217.032] | 1.000× | 0 |
| VectorWarp AUTO | 241.557 [235.430–242.810] | 0.895× | 597/600 |
| VectorWarp GPU | 238.168 [237.152–241.286] | 0.907× | 597/600 |

GPU ambiguity was numerically qualified (all completed array runs: zero
detection/track disagreement frames; RMS relative map error `1.1335e-6`, peak
`1.1901e-6`), but it made total pipeline time slower in this five-channel
profile. The median ambiguity stage was 102.068 ms on CPU, 142.578 ms on AUTO,
and 140.001 ms on explicit GPU; total-pipeline behavior, not ambiguity alone,
is the decision metric.

## Thermal-stopped partial hosts

These are single-run observations, not replicated matrices and not candidates
for a retry.

| Host / completed pair CPU pipeline | Upstream CPU pipeline | Observed ratio | Status |
| --- | ---: | ---: | --- |
| Strix AMD (`strix`) / 46.751 s | 47.690 s | 1.020× | Stopped at 82 C during array CPU; no retry. AUTO 37.601 s and explicit GPU 37.007 s completed as single pair runs. |
| HP Fedora (`fedora3`) / 107.571 s | 117.917 s | 1.096× | Stopped at its thermal cutoff during first AUTO-GPU run; no completed GPU result or retry. |

Strix’s completed single pair runs had zero detection/track disagreement frames
(one GPU run had one 0.01-unit rounding-only frame). HP’s two completed CPU
pair runs had zero disagreement frames. Thermal stops are outcomes, not failed
data to be replaced.

## Limits and interpretation

- The MCHQ TCP packet order and all recorded metadata were verified, but the
  original Kraken stream has no hardware sequence counter; sample-gap freedom
  of the capture itself is not claimed.
- Correctness comparisons use full complex-map validation outside measured DSP;
  validation is separately timed. JSON comparison tolerates only the documented
  one-frame 0.01-unit rounding case and excludes track identifiers.
- NVIDIA is the only complete replicated performance matrix. Strix and HP are
  intentionally partial thermal-stop evidence.
- No deployment, model promotion, radio operation, or claim about live capture
  throughput follows from this offline campaign.
