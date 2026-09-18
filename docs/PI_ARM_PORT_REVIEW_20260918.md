# Pi ARM port review — 2026-09-18

Initial source review is complete. No DSP, capture, API, installed service, or
OS change has been made in this stage. The separate implementation branch is
`codex/pi-arm-improvements`, based on VectorWarp main
`a1fe4cdd647030823e6ae20813717ac0081e09cd`. The active macOS checkout is separate.

## Sources and limits

- [blah2-arm PR #67](https://github.com/offworldlabs/blah2-arm/pull/67): open,
  head `e2527aaabd42931ae23275c7bac1cdf95d22c01c`, base
  `9f23215eddfba378f4e07c0456a6d9cf8e4afb5c`.
- [Companion owl-os PR #57](https://github.com/offworldlabs/owl-os/pull/57):
  closed without merge; head `ec241387bc614a634c831ba4a7514f817c2547cd`.
- PR67's base already includes upstream PR64 FFT padding, PR65 output/copy/CFAR
  improvements, and PR66 CPI pipelining. Those are not new PR67 contributions.
- PR67 reports 4,781 live CPIs over 40 minutes, mean 408.263 ms, p99 448.026 ms,
  maximum 488.028 ms, and no 500 ms deadline misses or observed capture losses.
  This applies to the complete tested OWL stack at two channels, 2 MS/s,
  500 ms CPI, delays -10 through 400, Doppler ±300 Hz, and 410 clutter taps.
  It is not a measured improvement over current VectorWarp. VectorWarp's older
  published 2.4 MS/s / 200 ms Pi results use a different workload and OS.
- Locally reran PR67's existing `node api/test_json_frames.js`: all 84 UTF-8
  fragmentation cases, reconnect isolation, bounds, and backpressure retry
  passed. No new DSP benchmark or hardware load test ran during this review.

## What is worth porting

| Priority | Candidate | Adaptation needed in VectorWarp |
| --- | --- | --- |
| First | Blocked clutter correlation and overlap-save FIR | Replace whole-CPI FFT work in `WienerHopf.cpp` with small reusable blocks; retain signed delay and linear-filter semantics. Evaluate the persistent correlation worker within VectorWarp's thread budget. |
| First | Shorter ambiguity FFT geometry | PR67 uses `nCorr + max(abs(delayMin), abs(delayMax))` for requested lags, versus VectorWarp's full `2*nCorr-1`. Preserve geometry validation and verify both CPU and GPU paths, nonzero Doppler centers, and boundary lags. |
| First | Folded spectrum transform | Compute regularly sampled spectrum bins with a smaller transform, retaining VectorWarp's bin labels, normalization, output length, and full-CPI definition. |
| First | Per-connection TCP JSON framing | Adapt PR67's `api/json-frames.js`. VectorWarp's listeners still concatenate chunks until the final character is `}`; coalesced JSON objects and reconnects can corrupt delivery. Keep current VectorWarp routes and data contracts. |
| Next | Paired CPI blocks and sample-clock accounting | RSPduo already validates paired callbacks, but capture still converts/copies under FIFO locks and overflow lacks explicit accounting. Port block ownership, whole-pair dropping, counters, and stop/reset handling. Keep other receivers and 2–8 channel operation intact. |
| Measure | Guarded Hermitian Toeplitz solve | Retain condition/residual checks and dense fallback. Earlier local live-RF evidence recorded 263/263 fast-path declines and about 7.6 ms per recorded-CPI attempt; synthetic solver gains are not enough to choose this by default. |
| Later | Pi V3D split FIR | Test the idea of CPU correlation/solve plus split CPU/GPU filtering through VectorWarp's existing isolated GPU worker, numerical qualification, and fallback mechanisms. The PR's in-process V3D-only backend and fixed 50% split are not a drop-in replacement. |

## Important compatibility details

- VectorWarp already reuses FFT plans/workspaces, uses fast convolution padding,
  has complete application socket writes (`WriteAll.h`), and updates browser
  histories from completed frames. Import only missing behavior.
- Spectrum requires an intentional port: PR67 uses `floor(n/decimation)` bins,
  truncates its transform to complete groups, and preserves `(nfft/2+1)` bin
  selection. VectorWarp uses the full input, `ceil(n/decimation)` output bins,
  and `(nfft+1)/2` shift. Start with an exact divisible-length fast path and
  retain the current transform for unsupported geometries.
- PR67's correlation worker is persistent; its optional ambiguity row worker
  is created and joined per call. Do not describe every worker as persistent.
  VectorWarp already parallelizes surveillance paths, so blindly adding two
  workers per path could oversubscribe the Pi's four cores.
- FFTW planning changes need coordinated ownership: PR67 clutter sets the
  global planner thread count to one; its local mutex does not coordinate
  other DSP classes. Preserve VectorWarp's chosen per-path/FFT thread budget.
- The SDK ratio-three counter adapter applies only to its validated 6 MHz ADC
  / 2 MS/s dual-tuner configuration. Do not generalize it to all sample rates.
- Current VectorWarp clutter destroys plans but does not release its raw
  scratch arrays. Use owned storage with exception-safe cleanup when replacing
  this class; include repeated construction/destruction checks.
- OWL boot, Mender, watchdog, and Compose-selection changes belong to that
  deployment. VectorWarp does not need an OS transplant to adopt the DSP.

## Verified remote test route

Read-only SSH through the existing Pi Zero relay succeeded on September 18:
Mac → ADS-B Zero 2 W → Pi 4B LAN SSH. The Pi4 reported the Zero's LAN address
as its SSH peer, confirming the requested route rather than direct Pi Tailscale.
No new Tailscale approval was needed.

Pi4: Model B Rev 1.5, aarch64, Debian 12 Bookworm, about 7.6 GiB RAM,
7.1 GiB available, 201 GiB free root storage and 12 GiB free under `/data`.
RSPduo USB and `/dev/dri/renderD128` are present; throttling is `0x0`.
SSH, NetworkManager, Tailscale, Docker, and SDRplay services are active.
The existing API/web/host containers are running; no radar container was listed.
Mesa is `24.2.8-1~bpo12+rpt5`.

The current host lacks a complete native build toolchain (CMake/Ninja were not
found). Use a separate Debian-12-compatible build/container before considering
OS changes. Existing packaged Debian-13 binaries are not the starting point.
Keep the working boot, LAN SSH, and Zero relay available throughout testing;
the user cannot physically replace the SD card.

## Next acceptance checks

1. Record current-main baseline and immutable input/build/config hashes in an
   isolated test directory. Use the same true sample rate, CPI, channel count,
   thread budget, and recorded IQ for each candidate.
2. Port and validate one candidate at a time. Compare complex maps, detections,
   spectrum axes/amplitudes, and tracker behavior; include negative/edge delays,
   odd/nondivisible spectrum sizes, zero and ill-conditioned input, and 2–8
   channels. Keep existing numerical thresholds and CPU/GPU fallback tests.
3. Exercise partial/coalesced UTF-8 TCP frames, reconnect isolation, bounded
   buffers, queue overflow, tuner mismatch, sample-clock wrap, reset, and clean
   shutdown with work outstanding. A delivered-frame count is not sufficient
   evidence of capture continuity.
4. On Pi4, alternate baseline/candidate runs and record stage time, p50/p95/p99,
   maximum, deadline misses, RSS, temperature, throttling, and capture losses.
   Distinguish end-to-end latency from pipeline output cadence. Do a live RSPduo
   endurance run only after recorded-IQ correctness passes.
5. Retain only independently supported gains, then measure the combined port.
   Preserve the working installation until the candidate passes these checks.

Local continuity artifacts (not for public packaging): PR67 source and metadata
are under `../pi-arm-review/`; the existing relay helper is
`/Users/michaelslaven/Projects/owlos/connect-pi-via-adsb.sh`. Prior experimental
solver evidence is in that project's `benchmark/CURRENT.md`; final packaged
PR67 results take precedence over historical experiment timings.
