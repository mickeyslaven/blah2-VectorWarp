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

The current worktree adds a software-only host-receipt/sample-anchor diagnostic:
a robust rolling offset/drift fit reports residuals and starts a fresh relative
epoch on a receipt discontinuity. It remains explicitly inexact and unbounded;
see `docs/KRAKEN_TIMING_SOFTWARE_BOUNDARY.md`. It is not deployed and did not
alter live RF/configuration.

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
| 1a | Kraken timing: host-receipt/sample offset could jump or drift without quantitative provenance | Code/tests complete; not deployed | Rolling robust host-anchor fit, residual/drift provenance, and >100 ms receipt-step epoch break are diagnostics only; `docs/KRAKEN_TIMING_SOFTWARE_BOUNDARY.md` records the unbounded UTC blocker and 527 MHz mapping |
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
replacement candidate explicitly admits `examples`, `oprs`, and `schemas`.
Operational `results/` remain excluded with `export-ignore`, and the release
test rejects a synthetic results tree so ADS-B positions, RF settings, host
paths, hashes, and other site telemetry cannot enter a public source archive.
A second regression packages the complete current Git archive so future
top-level source additions fail during pull-request review instead of after
tagging. Repository policy prevents rewriting or deleting the unpublished
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
- Strix Flash-Next is controller-owned and must never be started directly with
  `systemctl`. A direct start correctly fails without a compute lease. The
  supported interactive lease path passed admission, runtime-provenance,
  model-readiness and normal-release checks on 2026-10-24, then retained the
  model as `FLASH_IDLE` with the model service active and the tailnet proxy
  inactive until claimed. Monitoring now checks controller readiness/residency
  instead of misclassifying the default heavy-batch admission snapshot, whose
  retired local-radar recorder evidence is irrelevant to interactive Qwen.
- The `v0.1.24` tag's Linux and macOS CI builds passed, but the local Mac
  signing/notarization agent correctly stopped before publication because the
  production npm lock had advanced `proxy-addr` from 2.0.7 to 2.0.8 while its
  reviewed source/notices baseline still named 2.0.7. The reviewed delta is
  exactly one lock entry: the package remains MIT, its LICENSE is byte-for-byte
  unchanged, and the 2.0.8 registry tarball is pinned by both npm integrity and
  SHA-256. Release input generation now rejects every other lock difference,
  verifies both packaged runtimes and their license bytes, updates the notice
  index, and includes the exact 2.0.8 source archive in corresponding source.
  Because release tags are immutable, this tooling correction must ship in the
  next patch tag; the `v0.1.24` draft remains unpublished.

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

## Detection-manifold implementation (2026-10-24, not deployed)

Pull request #74 (`codex/detection-manifold-runtime`) has completed independent
adversarial acceptance for merge/release as default-off infrastructure. The
feature remains unqualified for live activation, and the live 527 MHz
configuration/release has not changed.

The intended detector has two distinct measured inputs: a B-by-C bank of
aircraft responses and a separate C-channel fixed-transmitter anchor. Runtime
weights use `current_reference[c] * aircraft_response[b,c] /
direct_anchor[c]`, followed by gain normalization, the configured direct-path
projection and then optional diagonal noise whitening. The anchor is never a
searched aircraft beam. CPU and GPU paths must share this ordering so an A/B
cannot appear better merely because the ideal and measured arms applied the
null differently.

The first adversarial review found that the initial implementation was not
releaseable: its native tool consumed caller-supplied post-CAF vectors while
claiming raw-IQ equivalence; the fitted fixed-transmitter anchor overwrote the
nearest aircraft beam; JavaScript identity v3 did not match the C++ v2 loader;
the detection artifact schema was not loadable by the runtime; global elevation
support admitted unobserved azimuth/elevation combinations; actual gain epochs
could be mixed; candidate RF/corpus identity and baseline labels were not fully
bound; and ideal/measured GPU comparison used different null/whitening order.

Repairs now implemented include: diagnostic-only naming for the post-CAF
native tool; an acceptance boundary that remains closed until an authenticated
raw-IQ exporter exists; pass-balanced fixed-transmitter anchoring; a separate
anchor that does not consume an aircraft beam; common observed elevation
support across every retained aircraft beam; actual per-channel gains in the
v3 installed identity; exact candidate RF/rate/channel/config/corpus binding;
baseline-label consistency; and a shared GPU calibration helper. The bank hash
binds both aircraft responses and the separate direct response. Schema-specific
C++ loaders prevent bearing/detection artifact confusion, match the JavaScript
v3 fingerprint exactly, and retain the per-frame exact actual-gain check. The
resident transport is now ABI/protocol 11 with config schema 3; ABI10 peers
cannot accept the new anchor field. Replay-only artifacts are explicitly
rejected by the live-production load call.

Focused acceptance passed on macOS: the real loader consumed a generated
detection JSON artifact; the CPU bank and resident helper retained the aircraft
beam nearest the transmitter; the separate anchor survived float conversion
and wire round-trip and changed the config hash; invalid anchor shapes/values
failed closed; `testKrakenMultiChannel` passed 9,369 assertions/27 cases,
resident wire passed 190 checks plus 101 rejection checks, resident processing
passed 49 checks, frame diagnostics passed, the native diagnostic test passed,
and `blah2` built. This is software acceptance, not physical aircraft evidence.

A second independent acceptance review found two additional integrity defects
before publication. First, eight-decimal gain serialization could change a
binary32 tuner gain after JSON reload while the runtime correctly required an
exact gain epoch. Manifold evidence now uses lossless RapidJSON output, trusted
manifold/native inputs use full-precision number parsing, and the GPU-result
suite exercises the complete evidence-serialization -> identity-fingerprint ->
C++ loader -> actual-gain-validation round trip using widened `14.2f` and
`14.1f` values. Second, the post-CAF diagnostic consumed a separate direct
anchor without exporting it or proving that its declared bank hash covered it.
The native tool now recomputes SHA-256 over both `direct_response` and
`responses`, rejects either kind of tampering, exports the exact consumed
anchor, and uses the recomputed identity in every result. A fresh post-fix
review then demonstrated that JSON number rendering is not cross-language
canonical: RapidJSON and `JSON.stringify` can print the same IEEE-754 value
differently. The bank identity therefore uses the explicit
`vectorwarp-detection-bank-ieee754-v1` contract (tag, little-endian beam/channel
dimensions, then little-endian binary64 direct-anchor and aircraft-response
components), reproduced independently by JavaScript, Python and C++.
The review also caught an unintended change to the active ideal beam bank's
null/whitening order. That change was removed: with no detection manifold the
resident path is byte-for-byte the established ideal algorithm, while the
default-off measured arm follows the same null-before-whitening order. This
preserves release behavior but also preserves a known limitation: unequal
whitening and final normalization can weaken the requested soft-null depth.
Changing that live behavior requires a separate positive-controlled A/B, not a
manifold-infrastructure release.
Detection replay validation now also requires an actually present complex
covariance matrix and positive sample count whenever a detection context is
claimed. Previously, mutually absent covariance fields could satisfy an
`undefined === undefined` support check and crash later scoring; fit-only live
positive samples may still omit the replay context as intended.

After those repairs, the focused gain and anchor regressions passed, the full
45-test native CTest suite passed serially, `testKrakenMultiChannel` again
passed 9,369 assertions, and the complete API, DOM, manifold and 11-case receipt
suites passed. One parallel CTest run transiently failed the pre-existing
bounded-recorder fixture; that fixture immediately passed alone and the full
serial 45-test suite then passed. A fresh independent post-fix adversarial
review passed after independently reproducing the gain, bank-tamper,
cross-language fractional-value, signed-zero and missing-covariance cases.
Physical qualification and authenticated raw-IQ production receipts remain
separate live-activation gates.

The API now has a separate, default-off detection-manifold campaign.  It shows
positive-control fit coverage independently from replay-diagnostic coverage,
including pass counts, azimuth/elevation support, unsupported gaps, nuisance
cells and miss/subthreshold passes.  Live detected cells are positive controls
only.  The earlier covariance-normalized replay selects a sparse response from
ADS-B truth and therefore is **diagnostic only**: it is not the production
full-bank maximum surface and its noise denominator is not the production CFAR
training mean.  It can never set `qualified:true`.

The current `detectionManifoldGridReplay` executable is explicitly a post-CAF
diagnostic. It does not read immutable raw IQ and cannot produce an importable
production receipt. Hash allowlisting cannot repair that provenance gap. A
future exporter must itself open the raw-IQ artifact, apply the production
reference/clutter/ambiguity/fusion path, bind its exact sample ranges and emit
the complete same-IQ target/training grid. Until then `qualified` and automatic
promotion remain false even when diagnostic replay improves.

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

The next asynchronous corpus evaluation admitted all eight v2 response samples
from the first post-enable aircraft pass (N523ME): 8 accepted, 0 rejected and
1 independent pass. The fixed-transmitter anchor simultaneously advanced to
69 samples. This first pass is in the pass-disjoint holdout partition, so it
does not yet increment training-sector or fit-bin coverage.

## 2026-10-24 integration-sensitivity live shadow

Implementation is in branch `codex/integration-sensitivity-shadow`. The live
API portion is deliberately scheduler-only: after recording integrity, exact
sample containment, and at least three admitted CPI truth rows pass, it writes
one immutable bounded job. It never launches replay DSP on the receiver node,
so live processor work added by the feature is zero. Queue depth and per-clip
wall time are hard-bounded and status is exposed at
`/api/detection/integration-shadow`.

The external worker compares 50/100/200/500 ms dwell on identical IQ across the
deployed fusion mode, ideal coherent, noise-whitened, noncoherent, and every
physical channel. A physical channel is excluded from its own synthesized
reference, including on two-channel systems. ADS-B is used only after detector
decisions for event-time scoring. One pre-fusion complex cutout per dwell also
reports fixed-cell noncoherent, trajectory-migrated noncoherent, and
common-phase-corrected coherent accumulation plus the phase-coherence budget.
IQ/config hashes, atomic results, configuration immutability and the external
wall-clock budget are enforced.

Regression evidence currently includes the API queue/admission/overflow/
deduplication suite, 2-channel reference isolation, event-time target/nuisance
separation, incomplete-truth rejection, coherent moving-target synthetic test,
configuration inventory persistence, and the full API pretest chain. A complete
local processor replay ran the worker on synthetic five-channel IQ in 5.67 s,
published an atomic result, added zero live processor work, and measured direct
pilot phase-increment coherence above 0.9999999999. This validates execution and
accounting, not aircraft sensitivity.

The independent adversarial review initially rejected this implementation for
real provenance, denominator, concurrency, and receipt-authentication defects.
Those findings are now corrected. Jobs bind the exact admission document,
native recorder sidecar, IQ, canonical config, config/receiver epochs, applied
gain vector, producer host, and receipt-key identity. The worker requires one
production arm, reports explicit detection/track denominators, emits no
short-dwell-biased winner, labels the target-informed complex diagnostic as
post-detection oracle-only, and refuses the live producer host. Result receipts
are HMAC-SHA-256 authenticated with an owner-only key; spool writers cannot
forge completion. Atomic slot files enforce `max_pending` across API processes,
and an old unpublished reservation can be reclaimed only after proving its
same-host owner PID is dead. Worker claims likewise use host/PID/binding-aware
bounded crash recovery. Hot scans are capped by archiving completed pairs.

Focused queue, worker, configuration and UI tests pass; the full API `npm test`
chain passes; CTest discovers and passes `integrationSensitivityShadow` in a
Kraken-only configuration. The final independent re-review passed with no
remaining P0/P1 release blocker, including the live-owner slot regression.

Deployment remains pending commit/release packaging and creation/distribution
of the owner-only receipt key. When deployed, the queue may collect immediately,
but Strix execution must wait for a controller compute lease and must not
contend with Qwen. A supported Qwen lease/review was attempted; the controller
returned conflict because its current admission state is not ready (including
the configured memory threshold), and the guarded service is currently
inactive. No guard was bypassed; the independent Codex red-team review was used
for release gating instead.

## 2026-10-24 527 MHz live and bearing-recorder checkpoint

The live chain is healthy on release
`/opt/vectorwarp/releases/20261024T145000Z-v0128-manifold-config-hotfix`:
Heimdall, API and processor are active, frames advance, the receiver readback is
527 MHz / 2.4 MS/s / five coherent channels with applied gains
`[2.7,2.7,1.4,1.4,3.7]` dB, GPU/clutter state is ready, and no rail or numeric
fault is present. ADS-B is online through both local feeds and adsb.fi;
adsb.lol is independently degraded and does not take the aggregate feed down.

The current one-hour provisional scorecard has 45 finalized eligible passes:
4 raw-detected, 3 tracked, 42 missed, and 1 detected-but-not-tracked. Thus raw
detection yield is 8.9% and tracking yield is 6.7%. These figures are
operationally useful but remain `decision_grade:false` because Kraken/Heimdall
does not provide a qualified RF UTC epoch.

The bearing-manifold recorder is active without error. It currently holds 15
accepted aircraft samples from 3 independent passes / 2 aircraft, concentrated
in the 315--45 degree azimuth region; no 45-degree sector or 60-beam fit bin is
ready and no measured bearing model is active. The separate fixed-transmitter
anchor has 699 JSONL rows (the status snapshot reported 658 accepted reference
samples) and remains highly repeatable, while its correlation with the ideal
manifold remains poor. The corpus must not be promoted until pass, aircraft,
sector, holdout and bin gates pass.

A diagnostic compatibility defect was found: the recorder writes
`vectorwarp-manifold-sample-v2`, while `evaluateBearing()` accepted only v1,
causing the diagnostic to falsely report no replay-capable samples. The local
fix accepts both schemas and has a focused v2 regression test; both bearing
diagnostic and cache test suites pass. It is not yet deployed in this
checkpoint.

## 2026-10-24 shared corpus / separate manifold cleanup

Bearing and detection are now described explicitly as two independent models
fed by one immutable installed-array response corpus. The canonical status
contract is `/api/array-calibration/status`; it separates the aircraft corpus,
the fixed-transmitter reference stream, bearing model state and detection model
state. The old bearing-prefixed endpoints remain compatibility views. The UI
uses the canonical contract and no longer implies that the detection runtime
candidate or reference anchor is part of the bearing model. A missing API field
that hid the detection runtime-candidate path was also corrected.

The bearing diagnostic now accepts both v1 and v2 recorder rows and uses the
recorded physical channel response and local covariance independently of the
upstream detection-fusion mode. This permits a covariance-MVDR bearing replay
while live detection uses noise-whitened fusion; it does not mix or activate a
model.

Two historical 527 MHz v1 corpora were copied into a non-rotating bearing-only
archive without changing the radar:

- `bearing-history/527mhz-bearing-v1-20261023-24.jsonl`: 993 rows, SHA-256
  `e6020fbce766772442b50c2ddabdaaa8eac54bb1a77704ec9f6b9ba538f871c8`;
- `bearing-history/527mhz-bearing-v1-20261023.jsonl`: 1,083 rows, SHA-256
  `bd8ebb997068b5534548d5cc35e956665b62a5407d2515695e8d175d28341fcc`.

These 2,076 rows are retained for bearing-only diagnostic evaluation. They are
not part of the live v2 corpus or detection fitting. Both lack the complete v2
applied-gain/identity contract, and the older block also lacks receiver-gain
metadata, so neither is eligible for automatic production promotion.

The cleaned bearing-only replay can read both archives even while live
detection uses noise-whitened fusion. The 993-row block covers 38 aircraft / 5
sectors; its best channel-order hypothesis has 29.24 degree aircraft-macro
median error and 63.2% within 45 degrees. The 1,083-row block covers 63 aircraft
/ 7 sectors; its best hypothesis has 35.94 degree median error and 57.1% within
45 degrees. The configured order's uncorrected medians are 128.06 and 115.74
degrees. These are useful diagnostic evidence of a large installed phase-axis
bias or multipath distortion, not authorization to change the user-confirmed
physical mapping or activate a model; the v1 rows lack the new identity and
timing contract.

## 2026-10-24 live scorecard denominator and geometry audit

The unusually low provisional headline was audited against the live 527 MHz
configuration and persisted CPI/pass evidence. The geometric prediction is
correctly three-dimensional: WGS84 ECEF receiver-to-aircraft and
transmitter-to-aircraft slant paths are used, and delay is excess bistatic path
length. Delay is independent of carrier frequency. Doppler is path rate divided
by wavelength and the live predictor is using 527 MHz; a focused regression now
proves that doubling carrier doubles predicted Doppler while leaving delay
unchanged.

The old headline was nevertheless not a defensible regression metric. It
treated the complete 62.457 km excess-delay map as homogeneous proven radar
reach, did not report receiver slant-range mix, counted the intentionally
suppressed region below the 0.625 km detector floor as eligible, and silently
discarded near-tied ADS-B associations. The below-floor defect affected 231
observed frames in the audited hour but did not explain most of the 71 reported
misses. Most misses had long opportunities (median 424 eligible CPIs), so a
real sensitivity/geometry problem likely remains after accounting is fixed.

A focused local correction now:

- separates map eligibility from detector-searchable eligibility;
- includes the detector floor in the frozen evaluation-contract identity;
- reports fixed excess-delay and three-dimensional receiver-slant strata;
- retains raw map-wide yield as descriptive only;
- reports ambiguous, unmatched and selected association points;
- computes a current-hour versus previous-hour pooled direct-standardized rate
  on common excess-delay x slant-range strata, requiring at least 80% common
  support before the geometry comparison can be decision-grade.

This directly prevents an influx of approximately 45 km aircraft from being
misreported as a detector regression. The standardized comparison is still
non-decision-grade on Kraken until exact RF event time exists. Focused geometry,
scorecard and syntax tests pass. Durable audit:
`results/20261024-live-scorecard-denominator-audit.json`. The correction is not
yet live; it must land as a separate follow-up after PR #78's timing/capture
boundary, then begin a fresh score epoch rather than rewriting historical data.

## 2026-10-24 living detection-manifold shadow acceptance

PR #87 at commit `eeeb321` now provides the bounded software path needed to
learn from geometry-selected aircraft passes without conditioning collection on
CFAR success. It exports signed native resident snapshots, maps them to admitted
source-sample time, authenticates and appends physical-channel complex target
and nuisance evidence, isolates exact RF/config/full-gain/dwell epochs, fits a
pass-balanced two-dimensional azimuth/elevation candidate, and writes immutable
candidate and held-out projection A/B receipts. The WPXI fixed-transmitter
stream remains a separate channel drift/frequency anchor and is never fitted as
an aircraft response. Unsupported cells remain unsupported; sparse cells shrink
toward their direction-specific geometric ideal. Automatic production promotion
is hard-disabled.

The adversarial review corrected phase-insensitive combining, coefficient
conjugation, common-phase alignment, elevation clamping, fabricated all-ones
ideal responses, holdout leakage, mixed dwell/config/gain epochs, unsigned or
truncated ledger acceptance, archive-import omissions, native-only startup
deadlock, artifact collisions, polling races and misleading A/B labels. Local
manifold/statistics/native-bridge suites pass. On Strix, exact commit `eeeb321`
passed the two native resident snapshot/bridge CTests, the complete manifold and
statistics suites, and an unchanged CPU replay of recorded r21 (7 detection and
7 track frames; result SHA-256
`a33ab8cde61ecbc30fee86b9cf031eda4ea72449d12b74982fbaebe79bab9932`).

This is accepted for shadow diagnostics, not yet deployed or production-
qualified. The remaining acceptance gate is a controller-admitted Strix GPU
resident replay on genuine admitted clips for 1/2/4/8 CPI windows with whitening
off/on, followed by an independently frozen pass-disjoint aircraft and
target-free cohort. The deployed live release still lacks
`/api/detection/integration-shadow`; its current aircraft corpus remains 30
samples from 3 independent passes, while the live WPXI anchor and frequency-
resolved shadow continue to advance. A two-hour thread heartbeat named
`VectorWarp radar and shadow audit` is active and must report every run until
the user responds.

## 2026-10-25 00:51 EDT live shadow checkpoint

The live radar remains on the v0.1.28 hotfix with configuration revision
`6daa446c...`. Frames advanced across the check, the receiver readback matched
527 MHz / 2.4 MS/s / five channels / gains `[2.7,2.7,1.4,1.4,3.7]`, GPU and
clutter processing were resident and ready, ADC rails were negligible, disk
usage was 33%, and ADS-B had three healthy sources while adsb.lol remained the
only degraded source.

The corrected-site corpus advanced to 73 accepted samples from 6 independent
passes / 5 aircraft, with zero invalid or mixed-fingerprint rows. Coverage is
still concentrated in two adjacent 45-degree sectors, with only one sector
meeting the bearing-fit pass threshold and no fit-ready 60-beam bins. The WPXI
anchor advanced to 1,413 samples and the frequency-resolved observer produced a
fresh sequence-36 result with no worker error. The current live release still
returns 404 for `/api/detection/integration-shadow`, so the truth-selected
native export and living-manifold A/B path in PR #87 are not collecting live
evidence yet. PR #87's required checks are green; deployment remains behind the
in-progress v0.1.29 release boundary and its Strix GPU replay gate.

The provisional one-hour scorecard counted 13 eligible passes, 3 detected and
3 tracked. Ten eligible passes had no radar return. This is current operational
evidence, not a new sensitivity conclusion.
