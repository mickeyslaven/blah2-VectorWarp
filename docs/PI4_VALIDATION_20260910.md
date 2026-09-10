# Raspberry Pi 4 / Fedora 44 validation — 2026-09-10

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

Pair map comparison reported zero relative RMS/peak error. Detection delay and
Doppler positions matched, but detection SNR differed in all ten frames. Track
outputs matched; no active tracks had matured during the ten-frame window.
The SNR discrepancy remains under separate diagnosis. Do not interpret these
timings as a correctness-qualified speedup or tracker-accuracy result.

The real V3DV GPU attempts in `auto` and `gpu` modes both reached the 30-second
startup timeout, logged CPU fallback and reported `gpu_frames: 0`. This is a
fallback result, not successful Pi GPU acceleration; the approximately 40-second
wall times include that failed startup.

Guard logs reached 47.225 °C (47.2 °C rounded) and reported zero kernel alerts.
Firmware throttling telemetry was unavailable, so absence of throttling is not
established. No receiver or VectorWarp service was started for these offline
checks. Release packages and the signed public repository remain unpublished.

Small evidence files reviewed: `package-replay-acceptance.txt`, the `full-cpu`,
`pilot`, `repeats`, `array` and `gpu` command/summary files, GPU fallback logs,
guard JSONL and the two pair output JSONL files. The latter confirm ten SNR
disagreement frames, zero detection-location/track disagreement frames and zero
active tracks. No benchmark was rerun for this documentation update.
