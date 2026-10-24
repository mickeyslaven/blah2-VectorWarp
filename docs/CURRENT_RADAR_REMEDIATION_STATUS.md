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
- Deployed release: `/opt/vectorwarp/releases/20261023T200500Z-gain-adsb-v1`
- Deployed correction: per-channel gain normalization before covariance,
  reference synthesis, clutter cancellation, and beamforming
- Deployed correction: ADS-B association now waits four CPIs, fetches the
  newest snapshot, and projects it back to the CPI time
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

This commit is tested but is **not deployed** on the live node yet. Deploying it
without a separate provisional mode would intentionally stop authoritative live
ADS-B scoring. The current live association remains useful operational evidence,
but must be labeled provisional rather than hardware-time-exact.

## Serial remediation ledger

| Order | Finding | State | Evidence / next acceptance check |
| --- | --- | --- | --- |
| 1 | Kraken/F1/F5: dequeue/TCP-arrival time presented as exact RF time | Code and tests complete; deployment decision pending | `096e7e4`; add an explicitly provisional UI/scoring lane before live deployment, or add independently qualified time hardware/protocol support |
| 2 | RSPduo/F2: mid-CPI AGC gain changes are not epoch boundaries | In progress | Require fixed gain for scored coherent operation; deterministic GainChange discontinuity test |
| 3 | USRP/F3/F6: UHD `time_spec` continuity and coherent topology are not proved | Pending after RSPduo | Reject missing/jumping device time; attest clock/time source and supported topology |
| 4 | dual HackRF/F4: asymmetric callback/FIFO loss can silently shift pairing | Pending after USRP | Any unequal drop/progression must invalidate both channels and restart a qualified epoch |
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

## Supporting artifacts

- `docs/SDR_OS_PIPELINE_ADVERSARIAL_AUDIT_20261023.md`
- `results/20261023-gain-adsb-deployment.json`
- Commits `37e1bf4`, `aba2e45`, `5e4abac`, `096e7e4`

