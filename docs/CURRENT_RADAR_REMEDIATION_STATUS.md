# Current radar remediation status

Last updated: 2026-10-24 (America/New_York)

This file is the durable source of truth for the SDR/OS pipeline remediation.
Automation prompts and chat summaries are secondary; an important limitation is
not considered resolved until its code, tests, deployment state, and residual
risk are recorded here.

## Live node

- Frequency: 527 MHz WPXI
- Sample rate: 2.4 MS/s
- Receiver: five coherent Kraken channels
- Physical gains: `[2.7, 2.7, 1.4, 1.4, 3.7]` dB
- Config revision: `54db8e29db9aabf295851abb47db54efe34b0e9502d5e88c8fa5d43e5b659ccd`
- Deployed release: `/opt/vectorwarp/releases/20261024T005000Z-eventtime-provisional-v2`
- Deployed correction: per-channel gain normalization before covariance,
  reference synthesis, clutter cancellation, and beamforming
- Deployed correction: ADS-B association now waits four CPIs, fetches the
  newest snapshot, and projects it back to the CPI time
- Deployed correction: Kraken publishes coherent epoch/sample ranges and
  explicit inexact event-time provenance. Authoritative scoring rejects those
  CPIs; `/api/live-scorecard/provisional` retains clearly non-decision-grade
  operational visibility.
- RF, gain, mapping, detector, tracker, and null configuration were not changed
  by those software corrections.

## Critical timestamp truth

KrakenSDR/RTL2832 does not provide a hardware UTC timestamp with its samples.
Heimdall V2 provides coherent sample order but no device UTC epoch. A wall-clock
estimate derived from a USB callback or TCP arrival can remove software queue
jitter, but cannot become an exact hardware RF timestamp. Network, USB buffering,
host scheduling, and wall-clock discipline remain part of its uncertainty.

Commit `096e7e4` is the fail-closed correction:

- CPI midpoint is derived from coherent sample positions.
- Timestamp provenance is published explicitly.
- Kraken live/replay time is marked `exact:false` with unbounded uncertainty.
- ADS-B/OPRS consumers that require exact event time reject it.
- Playback wall time is no longer silently presented as RF time.

This correction is deployed. The authoritative scorecard is now fail-closed and
reports `decision_grade:false`; the separate provisional endpoint and UI remain
available but cannot be used for acceptance decisions. Live validation observed
the expected coherent sample range and `exact:false`, `uncertaintyBounded:false`
provenance while the radar continued processing at the unchanged RF settings.

## Serial remediation ledger

| Order | Finding | State | Evidence / next acceptance check |
| --- | --- | --- | --- |
| 1 | Kraken/F1/F5: dequeue/TCP-arrival time presented as exact RF time | Deployed with fail-closed authoritative lane | `096e7e4`, `b02ba18`, `ccbef6b`; exact acceptance remains blocked until Heimdall supplies qualified RF time |
| 2 | RSPduo/F2: mid-CPI AGC gain changes are not epoch boundaries | Code and tests complete | `87952c3`; every GainChange creates a hard epoch boundary, invalidates stale work, and stops recording. Residual boundary is callback-aligned (`sampleAligned:false`) because SDRplay supplies no sample counter |
| 3 | USRP/F3/F6: UHD `time_spec` continuity and coherent topology are not proved | Code and tests complete | Missing, regressing, or jumping device time stops before append; scored capture is limited to an attested single-motherboard B210 with two distinct subdevices. Device time remains non-UTC and exact ADS-B scoring fails closed |
| 4 | dual HackRF/F4: asymmetric callback/FIFO loss can silently shift pairing | Code and tests complete | Callback blocks publish only as matched pairs; short/asymmetric/progression/drop faults clear both FIFOs and leave the new epoch unqualified until hardware-trigger restart. Relative epoch is recorded, but no RF/UTC time exists |
| 5 | Recording/F7: `.blah2iq` omits gain, receiver, epoch, and gap provenance | Pending after backend fixes | Versioned manifest/header with replay round-trip tests |
| 6 | Linux/F8: validation accepts paths blocked by packaged systemd sandbox | Pending after recording | Fedora then Debian/Ubuntu/Pi package tests with the actual service restrictions |
| 7 | macOS lifecycle parity | Pending after Linux | Homebrew/standalone replay and provenance conformance suite |

## Completed validation

- Native Kraken suite: 9,323 assertions in 16 tests passed on Linux/Strix.
- Full API test chain passed.
- Unequal-gain live/replay parity and gain-metadata mismatch rejection passed.
- Positive r215 retained its detection: best margin `+5.127` to `+5.290` dB,
  with `2/6` frames above +3 dB in both versions.
- Miss r15 was not converted into a false recovery: best margin `-1.975` to
  `-2.085` dB, with `0/7` frames above +3 dB in both versions.
- Kraken timestamp-provenance change passed three Node suites, two Kraken
  recorder/socket CTests, a native processor build, and the complete 18-case
  offline replay matrix.
- Live exact/provisional separation passed its API regression and deployment
  checks. The exact endpoint rejected inexact Kraken timing and the provisional
  endpoint remained explicitly `decision_grade:false`.
- RSPduo gain-boundary changes passed mocked SDK, paired-queue, API schema, SDRplay
  3.15-header, and full processor build checks. This backend is not deployed on
  or exercised against the live Kraken node.
- USRP device-time/topology changes passed deterministic missing/regressing/
  jumping timestamp and topology tests, API provenance validation, and a full
  processor plus receiver-module build against Homebrew UHD 4.10 headers. No
  radio hardware was opened and the live Kraken node was not changed.
- Dual-HackRF pairing changes passed deterministic one-sided FIFO overflow,
  short callback, callback-progression divergence, paired recording/status API,
  real libhackrf module, and full processor build checks. No radio hardware was
  opened and the live Kraken node was not changed.

## Supporting artifacts

- `docs/SDR_OS_PIPELINE_ADVERSARIAL_AUDIT_20261023.md`
- `results/20261023-gain-adsb-deployment.json`
- `results/20261024-eventtime-provisional-deployment.json`
- Commits `37e1bf4`, `aba2e45`, `5e4abac`, `096e7e4`, `b02ba18`,
  `ccbef6b`, `87952c3`

