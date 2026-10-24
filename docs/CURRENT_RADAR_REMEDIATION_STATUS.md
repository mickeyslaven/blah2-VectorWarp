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

- On 2026-10-24 Strix Flash-Next was restored through its supported controller
  lease path, not by starting the guarded service directly. Interactive
  admission, runtime provenance, model readiness and normal release passed;
  the model is resident as `FLASH_IDLE`, its model service is active, and the
  tailnet proxy remains inactive until claimed. The default heavy-batch
  admission snapshot still contains retired local-Strix radar-recorder state
  and is not the interactive-readiness signal. The two-hour monitor now checks
  controller readiness/residency and must never bypass the compute lease.
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

## Detection-manifold implementation (2026-10-24, not deployed)

The measured installed-array response can now be consumed by both the existing
CPU coherent beam bank and the GPU-resident coherent/noise-whitened detection
paths.  The resident route transports the full complex beam-by-channel matrix
through a versioned ABI and uses the same target/direct response-ratio
calibration as `CoherentBeamBank`.  Whitening is applied before the calibrated
direct-anchor projection, so unequal channel scales cannot reintroduce the
fixed-transmitter response; a 40 dB-null regression covers this ordering.  A
v2 installed-state identity binds RF and sample rate, channel count/order,
requested and observed gains, element coordinates and orientation, receiver
and fixed-transmitter coordinates, a canonical compatibility fingerprint, and
the recomputed direct-path bearing/beam.  Mismatches fail closed.  The feature
remains default-off; the live 527 MHz configuration and release were not
changed.

The API now has a separate, default-off detection-manifold campaign.  It shows
positive-control fit coverage independently from replay-diagnostic coverage,
including pass counts, azimuth/elevation support, unsupported gaps, nuisance
cells and miss/subthreshold passes.  Live detected cells are positive controls
only.  The earlier covariance-normalized replay selects a sparse response from
ADS-B truth and therefore is **diagnostic only**: it is not the production
full-bank maximum surface and its noise denominator is not the production CFAR
training mean.  It can never set `qualified:true`.

The API now also validates a strict
`vectorwarp-detection-manifold-production-replay-v1` import receipt.  A receipt
must bind the exact positive corpus and candidate hashes, independent IQ/replay/
input-grid/generator hashes, frozen frequency/sample-rate/configuration and
actual per-channel gains.  It must prove that baseline and candidate evaluated
every beam on the same complete CUT/training grid and retain raw CUT power,
training mean, production alpha, threshold and decision for one target plus
local nuisance CUTs.  The importer recomputes alpha, threshold and decisions and
rejects truth-selected beams, incomplete support, reused captures/grids and
candidate hash mismatches.  The trusted producer must be operator-pinned and
allowlisted by SHA-256; candidate and ideal banks are also bound by canonical
content/geometry hashes.  Candidate artifacts remain replay-only and automation
cannot promote them or mutate radar configuration.

The final adversarial pass closed receipt/provenance defects before
publication.  The Python receipt carries the native geometry/direct-path fields
required by the JavaScript importer and uses that importer's exact JSON-number
canonicalization (including native C++ values rendered as `.0`).  The native
export carries the exact measured response matrix it consumed, and the receipt
rejects any matrix that differs from the immutable candidate artifact even if a
caller preserves the old declared hash.  Full capture gain vectors are mapped
through `surveillance_channels` while retaining the original capture vector.
The strict receipt also derives and binds selected element positions, confirmed
orientation, receiver/fixed-transmitter coordinates, direct-path azimuth, null
depth, beam count, and exact CFAR guard/training geometry from the frozen config.

The default-off native `detectionManifoldGridReplay` tool now supplies the
missing production-equivalent export.  It uses `CoherentBeamBank` and the
production Gamma CA-CFAR primitive to emit every baseline/candidate beam for
the identical target and training-cell grid, with no truth-selected beam.
Sparse fits can become a dense replay artifact only when one observed elevation
band contains every azimuth bin; there is no interpolation, clamping, or
extrapolation.  Synthetic end-to-end tests cover native export through strict
receipt validation and reject incomplete training grids.  This proves the
software contract, not aircraft performance.

The native replay tool fails closed outside the statistic it currently
implements: unwhitened `coherent_beam_bank`, one CPI, no motion/TBD, and exact
configured guard/training geometry.  It will not label a single-vector replay
as production-equivalent to noise-whitened or multi-CPI processing.  The live
resident path does implement measured noise-whitened weights and the calibrated
direct-path null, but qualifying that mode requires a later native replay input
contract carrying its covariance/whitening and temporal evidence.

`script/evaluate-detection-manifold-replay.py` supplies an isolated same-IQ
diagnostic lane for ideal, coherent, noise-whitened, measured, MVDR/LCMV,
direct-plus-reflection constrained, and experimental frequency-resolved arms.
It preserves raw scores/ranks/margins and support masks.  Its acceptance check
now uses raw empirical p-values with full-search familywise correction at
alpha 0.05; the earlier tautological corrected-p check is covered by a failing
regression fixture.  ADS-B is scoring-only and never selects a beam or cell in
that tool.  The remaining acceptance check is a frozen pass-disjoint physical
positive/miss cohort with compatible frequency, installed-state and full gain-
vector fingerprints.  Only a dense manifold that passes that held-out replay
may be considered for a separately authorized live A/B.

## 2026-10-24 live v0.1.28 collection deployment

The signed v0.1.28 Fedora package is installed on the live Kraken node. A
shipped configuration-validation defect blocked its new collection settings;
commit `18b2272` repairs the schema/semantic validation and adds persistence and
invalid-input regressions. The patch is published as PR #75 and is presently
hot-deployed at
`/opt/vectorwarp/releases/20261024T145000Z-v0128-manifold-config-hotfix`.

Safe Apply produced config revision
`7a61e0abb613cef8895c0e9fbc596f49f63705f4331d26816daafcfc79a489e0`
without changing 527 MHz, 2.4 MS/s, five coherent channels, mapping or physical
gains `[2.7,2.7,1.4,1.4,3.7]` dB. Frames advance and GPU-resident processing
is ready. Detection-manifold automation reports `collecting`; the first
verified checkpoint contained 20 fresh fixed-transmitter anchor samples and
zero admitted aircraft-response samples. The old 993-row v1 corpus was rotated
aside and is not mixed into the new identity epoch.

No measured detection manifold is loaded into production. The unchanged
ideal/noise-whitened 12-beam path with its 6 dB direct null remains active.
Candidate promotion stays false until fresh pass-disjoint corpus and replay
gates pass; this is live evidence collection, not a measured-weight performance
claim.

