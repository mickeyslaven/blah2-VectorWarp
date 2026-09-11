# Fixed-range processing comparison

These are the current matched performance results. Every timed profile uses
**527 MHz, 2.4 MS/s, delays -10 through 245 (256 bins), and a maximum excess
path of 30.603813 km**. Excess path is the extra transmitter-to-target-to-receiver
distance, not the aircraft's distance from the receiver. Clutter filtering uses
delays -10 through 200 throughout.

[Summary CSV](comparison.csv) · [Summary and stage timings](comparison.json) ·
[Readable report](../../GPU_BENCHMARK_20260911.md)

## Workloads and controls

- 82 timed runs, 1,640 complete CPIs, on three hosts and four physical GPUs.
  Each configuration/backend has two repeats in reversed execution order.
- Each run processes 20 CPIs. The first eight qualification/startup frames are
  retained in the receipts but excluded from steady statistics. Mean, p95 and
  deadline counts pool the remaining **24 frames**, including periodic CPU
  accuracy checks. Mean processing time below the CPI is not a zero-miss claim.
- Both engines receive the same recorded samples at the original sample clock.
  The common file is a whole-packet 20.002-second prefix of real five-channel
  Kraken IQ. SHA-256:
  `1e8d50a5fe62410ead9094d95a57af1d03414e87ac00b8861b75444c7aab12aa`.
  A 200 ms run consumes the first four seconds; a one-second run consumes twenty.
- Pair tests select reference channel 0 and surveillance channel 1. Array tests
  use all five inputs and four surveillance workers. Original blah2 has no
  corresponding five-channel mode; those are VectorWarp capacity tests.
- The baseline is actual
  [30hours/blah2 c821bee](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de),
  not the Kraken pull request. The harness uses its original DSP implementation
  and excludes unsafe Doppler scratch geometry before timing.
- VectorWarp uses the previously qualified v8 GPU build, source archive SHA-256
  `8ceb110d4b71854a653789696b17fd7a57a318ab58f6c27212f9899422b666c3`.
  The subsequent unified receiver-module/browser changes do not change these
  DSP or GPU kernels. Every run records the actual executable SHA-256.
- Complex map and fusion outputs must match the CPU oracle within `1e-4` RMS
  and peak-relative error. VectorWarp's documented SNR correction remains;
  this is not a claim of bit-identical detection SNR output.
- Pipeline timing covers extraction, reference processing, spectrum, clutter,
  delay-Doppler, fusion, detection, tracking and JSON. File reading, offline map
  comparison and startup are recorded separately. This is sample-clock-paced
  processing evidence, not live USB/network acquisition or an endurance test.

## Machines

| Host | CPU / GPU | Identical CPU allowance for each backend |
| --- | --- | --- |
| Strix | Ryzen AI MAX+ 395 / Radeon 8060S | Physical cores 0-7, 800%; pair: 1 worker × 8 FFT threads; array: 4 × 2 |
| NVIDIA laptop | Core i7-12650H / RTX 4050 Laptop | Physical cores 0,2,4,6, 400%; pair: 1 × 4; array: 4 × 1 |
| Pavilion | Core i5-7300HQ / HD Graphics 630 and Radeon Polaris 12 | Cores 0-3, 400%; pair: 1 × 4; array: 4 × 1 |

Strix has a 12 GiB process allowance; laptops have 3 GiB, without swap. BLAS and
OpenMP are limited to one thread. No fans, system thermal limits, CPU frequency
limits or other services were changed. Strix retained its compute lease and
fresh automatic-fan/global-guard checks. Observed peak monitored temperatures
were 80.875°C, 49°C and 82°C respectively; available RAM stayed above
107.9 GiB, 8.9 GiB and 8.35 GiB. Host/driver details also appear in the
[prior device qualification](../20260911/v8-all-device-confirmation.md).

## Receipts and reproduction

Each host directory contains the fixed contract, exact profiles and command
arguments, executable hashes, preflight geometry, every per-frame timing, and
combined run summaries. `comparison.json` includes actual GPU-frame counts and
stage timings. NVIDIA array AUTO returned some delay-Doppler frames to CPU;
those frames remain in the reported results.

`summarize.py DIRECTORY` reconciles the three host directories and regenerates
the comparison (Python 3.11 or later). `run_equal_range.py` records new runs in a
new output directory using the existing instrumented benchmark bundle. Use
the machine's own CPU, memory and thermal guards; it does not establish them.
The original benchmark harness and source provenance are linked from the
[main report](../../GPU_BENCHMARK_20260911.md).

All 82 timed runs completed successfully. The campaign's final deliberately
unsupported ±40 kHz probe returned the expected validation exit code 1 instead
of geometry JSON; the initial runner treated that final probe as an exception.
`check_limit.py` independently verified the exact rejection from both engines
on all three hosts, after timing. Its receipts are `limit-acceptance.json`.
The runner now handles that expected rejection explicitly. No failed or unsafe
processing run was turned into a timing result. The Pavilion's first attempt
also stopped on missing GPU wake/sleep telemetry; its existing verified guard
was used for a fresh complete run, retaining the failed attempt separately.

Full maps, logs and thermal traces remain on Strix under
`/var/tmp/vectorwarp-equal-range-20260911.hrSI4f`; source IQ is retained separately.
The original remote receipts remain at
`display1-worker:/var/tmp/vectorwarp-equal-range-20260911.jEeivY` and
`pavilion-worker:/var/tmp/vectorwarp-equal-range-20260911.xfkvKA`.

Wider Doppler at this same range is documented as
[future algorithm work](../../FUTURE_WIDE_DOPPLER.md), not an implemented claim.
