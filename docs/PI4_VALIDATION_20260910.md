# Raspberry Pi 4 / Fedora 44 validation — 2026-09-10

For the new combined CPU build and the three-way comparison with original
blah2 and Off World Labs' ARM fork, see [current Pi results](PI4_PERFORMANCE_20260911.md).

Package and correctness evidence remains relevant. Timings below are historical,
not part of the [current fixed-range comparison](GPU_BENCHMARK_20260911.md), and
do not establish a Pi speedup over blah2.

The Fedora 44 ARM64 RPM installed on a Raspberry Pi 4 Model B Rev 1.5 with
SELinux enforcing. All 16 offline processor replay/startup cases passed.
VectorWarp services remained disabled/inactive. This is Fedora-on-Pi package
evidence; Raspberry Pi OS and DragonOS installation remain unverified.

## Package and test provenance

- Package source: `9cfc783ca133308d28b73ef06cb66f89c9b3d367`, from
  [package workflow 34534957708](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/34534957708).
- Artifact: `vectorwarp-0.0.0-1.fc44.aarch64.rpm`, 35,812,538 bytes.
  SHA-256: `f8529be169d927e0fe7f6ffa7355966c2a13d3d2faa11563673693425072eeb6`.
  The manifest records the Kraken backend, GPU `auto` and private Node 24.21.0.
- Acceptance covered 13 replay cases plus three invalid-startup cases. Replaying
  RSPduo, USRP and HackRF recordings does not establish their live SDK support in
  this Kraken-backend package.
- Evidence stays on the test Pi under `/var/tmp/vectorwarp-pi-20260910/results`.
  Package identity is recorded in the adjacent `package-fedora44-aarch64`
  manifest. No IQ data is included in this report.

The timing harness was built separately from the installed processor. Its CMake
cache identifies `vectorwarp-9cfc783/bench` as its source. Recorded CPU binary
SHA-256 values in `bench-build/package-sha256.json` are:

| Binary | SHA-256 |
| --- | --- |
| `bench-upstream` | `c545224745a9410b235fe03df99ac6ce671f8fc4099554a0cd7eaed8d60a90d5` |
| `bench-fast` | `a4d93b9447f6f14690d0708b29a5de6f341e826bcfd4bf97b82061c6abae8967` |

## Recorded timing and correctness limits

The offline profile used 2.4 MS/s at 527 MHz, a 0.2-second CPI and ten frames
(4,800,000 samples/channel). Four-core runs used CPU affinity 0–3, four FFT
threads and one OpenBLAS/OpenMP thread. Values below are pipeline time divided
by ten; reading, startup and validation time are separate in the saved summaries.

| CPU profile | Upstream ms/CPI | VectorWarp ms/CPI | Interpretation |
| --- | ---: | ---: | --- |
| Two-channel pair, four cores | 917.031 | 902.441 | One comparison; 1.6% timing difference, not an accepted speedup |
| Five-channel array, four cores | No equivalent run | 3748.025 | VectorWarp-only result; three surveillance workers |

Across three two-core pair runs (affinity 0,1; two FFT threads), the upstream
median pipeline time was about 0.9% above VectorWarp's. Both tested profiles exceeded the 200 ms
CPI, so neither demonstrated real-time processing. These short offline runs do
not establish sustained live acquisition throughput.

Pair maps matched at their stored `complex<float>` precision, and detection
positions matched at saved JSON precision. Detection SNR differed in all ten
frames because upstream overwrites the delay-interpolated peak with the Doppler
peak; VectorWarp already preserves both. Of 60 detections, 29 increased by up to
0.61 legacy display-scale units and 31 were unchanged. These values are not
conventional physical SNR in dB or evidence of increased detection sensitivity.
The regression test passes VectorWarp and fails the pinned upstream negative
control. Track arrays were empty; matching tentative counts do not verify
established-track equivalence. Short, overlapping timing runs do not demonstrate
a reliable speedup, even with the numerical discrepancy explained.

The real V3DV GPU attempts in `auto` and `gpu` modes both reached the 30-second
startup timeout, logged CPU fallback and reported `gpu_frames: 0`. This is a
fallback result, not successful Pi GPU acceleration; the approximately 40-second
wall times include that failed startup.

A separate diagnostic source build localized the timeout to driver pipeline
creation for the first VkFFT plan (3000 points × 321 batches). Device creation,
buffers and shader-module creation completed, but `vkCreateComputePipelines`
did not return before the unchanged deadline. This does not establish an
infinite loop or identify a particular Mesa compiler defect.

The same Pi's V3D 4.2.14.0 GPU computed six small synthetic frames across two
three-frame tests (range 16, Doppler 9, five delay bins, one surveillance path).
An independent CPU correlation/DFT reference found worst relative RMS error
`1.37223e-7` and peak-relative error `1.50091e-7`. No CPU fallback could pass
these tests. This proves small-geometry GPU computation only: production-size
recorded-IQ GPU processing and a Pi GPU speedup remain unverified. The diagnostic
build used Mesa 26.0.3-4, Vulkan loader 1.4.341.0, glslang 16.2.0 and the existing
pinned VkFFT source on Fedora 44; it does not extend installed-RPM acceptance.
See [GPU diagnostics](GPU_DIAGNOSTICS.md) for the opt-in checks.

Guard logs reached 47.225 °C (47.2 °C rounded) and reported zero kernel alerts.
Firmware throttling telemetry was unavailable, so absence of throttling is not
established. No receiver or VectorWarp service was started for these offline
checks. Release packages and the signed public repository remain unpublished.

Small evidence files reviewed: `package-replay-acceptance.txt`, the `full-cpu`,
`pilot`, `repeats`, `array` and `gpu` command/summary files, GPU fallback logs,
guard JSONL and the two pair output JSONL files. The latter confirm ten SNR
disagreement frames, zero detection-location/track disagreement frames and zero
active tracks. No benchmark was rerun for this documentation update.

Follow-up evidence is under `/var/tmp/vectorwarp-pi-investigation-20260910`
on the Pi and Strix: `REPORT.md`, `REPRODUCE.md`, and the `evidence/` logs,
per-detection reconstruction and checksums. The additional SNR and fallback
regressions passed; the upstream negative control failed as intended. The
follow-up peak temperature was 44.3 °C. Installed packages, shared source,
services and hardware configuration were unchanged by that investigation.
