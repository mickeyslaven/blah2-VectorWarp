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
- Deployed release: `/opt/vectorwarp/releases/20261024T022833Z-scorecard-bounds-v2`
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

## UTC nuisance accounting

The operational lane no longer treats an inexact SDR timestamp as either exact
truth or an automatic radar miss. The provisional score contract now has an
explicit `truth.adsb.provisional_event_time_uncertainty_s` radius (default one
second, allowed range zero to five seconds). For an inexact CPI, association
gates are expanded by the aircraft's measured bistatic delay and Doppler rates
over that declared radius. Every accepted association records the allowance;
the live scorecard exposes the radius and labels the accounting method. This is
evaluation-only: ADS-B still cannot steer detection, tracking, cancellation, or
beamforming. Exact scoring remains separate, while operational scoring remains
available and explicitly `decision_grade:false`.

Deterministic regression coverage proves that a synthetic echo outside the
ordinary gate but inside the one-second rate-derived tube is retained, while the
ordinary exact gate is unchanged. Configuration validation and default profiles
cover Kraken, RSPduo, USRP, dual HackRF, and generic two-channel systems. This
fix is deployed in `/opt/vectorwarp/releases/20261024T022506Z-time-nuisance-v1`.
The unchanged config revision was re-attested, all three live services are
active, processor frames advance, and the provisional endpoint reports the
one-second radius and `bounded-event-time-rate-expanded-association-gates`.

The deployment exposed a separate long-soak defect: detailed finalized passes
and eligibility events could grow until the persisted scorecard exceeded its
loader safety limit. The repaired persistence contract keeps a bounded detailed
working set, compacts finalized CPI/truth histories, and rolls evicted lifetime
headline counts forward. A 501-pass synthetic restart-state regression remains
well below the loader cap. Live acceptance retained 46 detailed passes and 8
eligibility events in 3.29 MB, with at most 16 finalized CPI and truth rows per
pass; the scorecard reloaded without error and live frames continued advancing.

Backend-specific mitigation remains honest about the hardware boundary:

- Kraken uses coherent Heimdall sample positions and one host-arrival epoch;
  transport offset is carried as a nuisance radius rather than invented UTC.
- RSPduo uses vendor `firstSampleNum` for callback blocks. Because `GainChange`
  events have no event sample number, the ambiguous span is discarded and a
  new sample/gain epoch starts before publication resumes.
- Dual HackRF uses paired software sample progression. Any asymmetric callback,
  FIFO loss, or progression mismatch invalidates both sides and starts a new
  qualified common epoch; the API cannot provide RF-setting readback that the
  hardware/driver does not expose.
- USRP requires continuous UHD `time_spec` and preserves clock/time-source
  readback. Device time remains relative unless a separately qualified PPS/UTC
  discipline maps it to UTC; until then the same nuisance-radius contract is
  used rather than assuming that continuous device time is civil time.

## Serial remediation ledger

| Order | Finding | State | Evidence / next acceptance check |
| --- | --- | --- | --- |
| 1 | Kraken/F1/F5: dequeue/TCP-arrival time presented as exact RF time | Deployed with fail-closed authoritative lane | `096e7e4`, `b02ba18`, `ccbef6b`; exact acceptance remains blocked until Heimdall supplies qualified RF time |
| 2 | RSPduo/F2: mid-CPI AGC gain changes are not epoch boundaries | Code and tests complete | `87952c3` plus bounded-resume follow-up; every GainChange invalidates only ambiguous stale work, closes only the affected recording epoch incomplete, and automatically resumes CPI/recording publication on the next matched A/B pair. Output remains provisional and `sampleAligned:false` because SDRplay supplies no event sample counter |
| 3 | USRP/F3/F6: UHD `time_spec` continuity and coherent topology are not proved | Code and tests complete | Missing, regressing, jumping, or discontinuous UHD time rejects only the corrupt epoch, clears unpublished IQ, rolls recording incomplete, and automatically republishes after two continuous timed blocks. Status records degraded/requalifying state and counters. Topology remains limited to an attested single-motherboard B210; device time is non-UTC and exact ADS-B scoring remains unavailable |
| 4 | dual HackRF/F4: asymmetric callback/FIFO loss can silently shift pairing | Code and tests complete | Callback blocks publish only as matched pairs. A short/asymmetric/progression/drop fault discards the ambiguous epoch, rolls recording incomplete, drains one callback per radio, and automatically republishes only after two matched callback pairs. Recovery is bounded to eight consecutive faults. Relative time remains provisional because libhackrf has no device counter or RF/UTC timestamp |
| 5 | Recording/F7: `.blah2iq` omits gain, receiver, epoch, and gap provenance | Code and tests complete | Portable sidecar v2 records backend receipts, channel/RF settings, identity/topology, relative sample epoch, intervals/discontinuities, and configuration/code fingerprints. Replay validates it against the binary header and exposes explicit `legacy-unqualified` and `decision-grade` modes |
| 6 | Linux/F8: validation accepts paths blocked by packaged systemd sandbox | Fedora, Debian/Ubuntu and Pi code/tests complete | Packaged units declare the Linux service path policy. Recording is constrained to the shared writable root; relative and `ProtectHome`/`PrivateTmp` replay paths fail before restart, while readable `/srv` replay remains usable. Pi image verification checks both unit flags |
| 7 | Pi/F9: baked counter scale 3 disables other supported RSPduo modes | Code/tests complete | Pi profile now requests `auto`; SDK descriptors select guarded ratio 3 only for the qualified 2 MS/s geometry and otherwise select ratio 1. Explicit operator ratio 3 remains strict. Builder/verifier also attest the exact seven flags, processor-only `plugdev`, and Linux service policy |
| 8 | macOS lifecycle parity | Complete | Current-source processor build, v2 recording contract, all-backend provisional replay, 11 setting/format runtime cases, owned lifecycle/supervisor, local Kraken/RSP Apply, Homebrew release and standalone packaging fixtures pass. Physical non-Kraken hardware remains explicitly unqualified |
| 9 | macOS/replay acceptance assumed event-time provenance was Kraken-only | Corrected | Full replay now asserts the source-specific inexact contract for Kraken, RSPduo, USRP and HackRF and explicitly selects legacy-unqualified mode; the macOS runtime caller supplies receiver identity. The cold-start lifecycle fixture retains bounded diagnostics and a realistic readiness allowance |

The code-enumerated receiver-setting ledger is maintained in
`docs/SDR_SETTING_TRACE_MATRIX.md`. Kraken, RSPduo, USRP and dual HackRF are
complete; all four OS/package lifecycle passes are complete.

## Completed validation

### v0.1.24 pre-release hardening

The immutable `v0.1.23` candidate tag was cancelled before any release was
created or published. Its merge-only Homebrew job correctly rejected newly
tracked public source roots that were absent from the archive allowlist. The
replacement candidate explicitly admits `examples`, `oprs`, `results`, and
`schemas`, while a new regression packages the complete current Git archive so
future top-level source additions fail during pull-request review instead of
after tagging. Repository policy prevents rewriting or deleting the unpublished
candidate tag, so the corrected release advances to `v0.1.24`.

The release candidate adds permanent regression gates for defects found during
the cross-platform acceptance pass:

- API integration tests allocate ephemeral ports rather than deriving a fixed
  range from the process ID, eliminating parallel-run port collisions.
- Both installed macOS lifecycle suites select the package's pinned Node
  runtime even when `PATH` contains an intentionally unusable `node`; the real
  Homebrew package passed lifecycle, five-channel simulated Heimdall-to-API,
  crash-recovery, retune, replay, formula, and linkage checks on Apple Silicon.
- Every packaged Node service (API, health watchdog, config guard and processor
  preflight) is bound to the pinned private runtime. Explicitly-invoked
  JavaScript helpers are non-executable in the RPM payload, so dependency
  scanning cannot invent a host `/usr/bin/node` requirement.
- Source installs refuse package-owned integration paths, package first-install
  refuses an unmanaged `/opt/vectorwarp` footprint, and custom prefixes require
  `--no-systemd`. Debian/RPM directionality tests cover both collision orders.
- Linux package/source collision tests remain required in the Fedora/Debian CI
  jobs because their GNU `stat`, `realpath`, systemd, and package-manager
  semantics cannot be accepted from macOS fixtures.
- A release-matrix browser run exposed that a transient API timeout remained in
  the diagnostic state after a later successful refresh. Frame and view errors
  now clear only after the corresponding update succeeds; deterministic fake-
  clock and real-Chromium tests force one timeout and prove recovery.

Independent pre-release review passed the API and macOS universal/standalone
fixture suites and found no additional reproducible runtime defect. It requires
the Linux package jobs to pass before merge. No physical SDR was attached to the
Mac; the five-channel Kraken test used USB-isolated simulated IQ and did not
touch the live 527 MHz node.

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
- Kraken setting-trace follow-up rejects an observed per-channel frequency or
  gain change even when the Suite counter does not advance, rolls only the old
  portable recording incomplete, resumes at source sample zero, and preserves
  the already-applied DC-removal/gain-equalization contract. The loopback
  live-to-record-to-replay fixture opens no SDR and passed.
- Live exact/provisional separation passed its API regression and deployment
  checks. The exact endpoint rejected inexact Kraken timing and the provisional
  endpoint remained explicitly `decision_grade:false`.
- RSPduo gain-boundary changes passed mocked SDK, paired-queue, API schema, SDRplay
  3.15-header, and full processor build checks. This backend is not deployed on
  or exercised against the live Kraken node. The bounded-resume regression
  additionally proves that two consecutive vendor gain callbacks leave only the
  overlapping recording epochs incomplete, publish the next matched CPI, start
  a fresh portable recording lazily, and retain provisional `sampleAligned:false`
  gain provenance. The setting-trace follow-up preserves the construction-time
  USB transport, SDK diagnostic level, counter scale, SDK-reported ADC and
  descriptor clocks, and a delivered-sample-relative epoch in status and
  recording. The mocked receive loop proves every supported output-rate/IF/
  decimation combination and per-tuner RF/gain/notch application. Independent
  physical RF readback remains unavailable and is never claimed.
- USRP device-time/topology changes passed deterministic missing/regressing/
  jumping timestamp and topology tests, API provenance validation, and a full
  processor plus receiver-module build against Homebrew UHD 4.10 headers. No
  radio hardware was opened and the live Kraken node was not changed. The
  bounded-resume follow-up additionally proves overflow/time fault, explicit
  requalifying telemetry, two-block automatic requalification, post-fault CPI
  publication, incomplete old recording epoch, and provisional replay of the
  clean successor epoch. Persistent failure remains bounded to eight retries.
  The setting-trace follow-up closes a status/UI gap: validated per-channel UHD
  frequency, rate, gain, antenna and subdevice getter values plus motherboard/
  RX identity now survive API validation, appear in Settings, and remain in
  recording provenance. Conflicting channel motherboard identity rejects
  startup; software readback is never presented as physical RF or UTC proof.
- Dual-HackRF pairing changes passed deterministic one-sided FIFO overflow,
  short callback, callback-progression divergence, paired recording/status API,
  real libhackrf module, and full processor build checks. No radio hardware was
  opened and the live Kraken node was not changed. The bounded-recovery follow-up
  additionally proves fault-to-drain-to-two-pair requalification, post-fault CPI
  publication, incomplete old recording, automatic clean successor recording,
  provisional replay, and an eight-fault terminal retry budget.
  The setting-trace follow-up preserves unique full serial-to-role resolution,
  per-radio setting acceptance and libhackrf's derived baseband-filter policy
  through validated status, Settings and recording provenance. These are
  explicitly not presented as RF-setting, external-wiring or hardware-time
  readback.
- Portable recording provenance passed deterministic Kraken, RSPduo, USRP, and
  HackRF-shaped live-to-record-to-replay round trips, missing/legacy sidecar,
  receiver mismatch, settings-fingerprint tamper, and decision-grade rejection
  tests. The actual USRP and HackRF mocked receive-loop fixtures, Kraken socket
  fixture, and Kraken-only processor build also passed. Existing v1/no-sidecar
  files remain operational as `legacy-unqualified`; all current live backends
  remain best-effort/provisional because none attests exact bounded UTC RF time.
- Fedora/RPM path-policy checks passed the deterministic validator matrix:
  packaged recording writes remain within the API/processor shared writable
  root, `ProtectHome`/`PrivateTmp` replay paths and symlink escapes are rejected,
  and readable `/srv` replay remains allowed. Both installed units explicitly
  opt into the policy; the disposable Fedora package fixture is the final
  release-host acceptance check and cannot run on this macOS development host.
- Debian/Ubuntu retained distro-owned HackRF `plugdev` permissions without a
  competing udev rule, while the API account remains outside the device group.
  The APT transaction suite (10 tests), install-document suite (15 tests), and
  shared path/group/rule contract passed. Full DEB/systemd/udev acceptance is
  intentionally confined to the disposable Linux package job.
- The Raspberry Pi image profile no longer makes alternate supported RSPduo
  output rates fail merely because its systemd drop-in requested counter ratio
  three. Mocked SDK coverage proves `auto` resolves to 3 for the qualified
  6 MHz ADC / 2 MS/s dual-tuner mode and to 1 for 1 MS/s without stopping
  capture. API/UI/recording provenance preserves policy and effective ratio.
  Offline image-contract tests also prove the exact seven settings, processor-
  only HackRF `plugdev` access, and packaged Linux path-policy flags. Full image
  assembly, boot, USB hardware and RF throughput remain arm64 release-host/card
  acceptance tests.
- The macOS pass rebuilt the current Kraken-only processor and passed the
  portable recording contract plus all 18 all-backend replay cases and 11
  settings/legacy-format runtime cases. The acceptance suite formerly rejected
  valid RSPduo/USRP/HackRF `eventTime` objects merely because it still assumed
  that provenance was Kraken-only; it now validates each backend's explicit
  source, `exact:false`, unbounded uncertainty, zero unavailable legacy sample
  position and nonempty reason. Lifecycle, Apply, Homebrew and standalone
  packaging suites pass. This is software/replay evidence, not a new physical
  non-Kraken or Intel qualification.

## Supporting artifacts

- `docs/SDR_OS_PIPELINE_ADVERSARIAL_AUDIT_20261023.md`
- `results/20261023-gain-adsb-deployment.json`
- `results/20261024-eventtime-provisional-deployment.json`
- `results/20261024-time-nuisance-deployment.json`
- Commits `37e1bf4`, `aba2e45`, `5e4abac`, `096e7e4`, `b02ba18`,
  `ccbef6b`, `87952c3`

