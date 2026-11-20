# Live and recorded-IQ processing parity

Current architecture/acceptance reference for maintainers and replay reviewers.
The contract is identical post-ingress processing for identical canonical samples,
configuration, numerical representation and selected qualified backend. It is not
a claim that different hardware, CPU/GPU arithmetic, acquisition losses, or elapsed
wall-clock cadence are bit-identical. Static tests are not physical qualification.

## Ownership and parity matrix

| Input or mode | Canonical owner after transport | Required invariant |
| --- | --- | --- |
| Kraken/Heimdall, USRP, RSPduo, HackRF | `src/capture/` adapters → canonical IQ | Receiver identity controls I/O, channel pairing, acquisition identity and decoding, not detector/tracker mathematics. Recorded roles override a dormant live receiver profile. |
| Native/legacy recorded IQ | `Replay`, recording/source adapters → the same main consumer | Recorded sample rate, carrier and channel roles describe the input; they cannot select alternative post-ingress stages. Unauthenticated legacy input is not calibrated geographic evidence. |
| Direct CPU/portable GPU | `RawIqStages`, `RadarPipeline`, `ProductStages` | Same configured reference → clutter → ambiguity → fusion → temporal integration → detection → tracking order and immutable `RadarPlan`. Disabled stages stay disabled. |
| Direct resident GPU | Qualified private resident ABI, canonical plan/product contracts | Same requested policy and ownership; retain native precision, geometry/capacity/refusal and calibration requirements. Unsupported explicit routes refuse. |
| Channelized CPU/portable/resident | Conditioning/channelizer → `CanonicalSignal` → `CanonicalRadar`/`RadarBank` | Channelization is ingress, not another radar. Lane rate/carrier normalization and stage owners are shared. Float/borrowed storage does not create an alternative inference policy. |
| Python production replay runners and UI replay selection | Shared `script/replay_session.py`, same production executable | Deep-copy the complete recorded process configuration. No implicit CPU selection, performance replacement, legacy stripping, stage disabling, diagnostic fusion/axis/clutter/TBD/fractional/stationary retuning. Explicit backend edits preserve all other performance fields. |
| Qualification observer, complex cutouts, resident snapshots | Authenticated observations of canonical intermediate/products | No alternate detector, tracker or publisher. Explicit observation requests are allowed; absence must not change processing. Geometry, chronology and artifact identity remain bound. |
| Paired tracking-policy evaluator | `test/gpu/recorded_iq_qualification.py` | Exact artifacts, recording/acquisition, plan and config pins; only declared temporal/motion and M/N/delete policy differences. Same exposure/partitions/holdout; unmatched tracks are not asserted false. |
| Fixed-clutter-weight qualification | Private portable checkpoint control, production residual/FIR/merge kernels | Authenticated single-lane portable replay only. Identical unmodified prefix; capture actual warm-CPI FP64 initial and post-cast FP32 correction coefficients. Fixed arm applies those coefficients to each current reference; no subsequent covariance/factor/solve. Default ordinary replay/live continues per-CPI refitting. CPU/resident/multilane fixed controls refuse. |

The normal paths share `src/blah2.cpp` scheduling/publication and the canonical
product owners. Replay may read synchronously; live acquisition has bounded
queues, paired-input readiness, split-frame overlap and recovery deadlines.
Those transport differences must preserve sample-position/epoch lineage and
reset temporal state at actual discontinuities, not silently manufacture equal
exposure. GPU deadlines and fallback/recovery outcomes must be reported; matching
configuration alone does not certify that two runs executed the same backend.

## Canonical tracker admission and stationary memory

`AssociationContract.h` owns the C++/GLSL comb, ambiguity and stationary
arithmetic. Both trackers preserve strongest-only three-member Doppler/range
comb births, first-record equal-SNR ties, two-member range-ambiguity promotion
holds, and the two-surviving-prediction ridge-sibling limit. Claimed detections
are excluded from birth comb populations. Ordered detector records and native
map axes are shared; nearest-coordinate ties select the lower native cell.
All valid first, interior and final detector columns remain eligible for
association and birth, including signed delay axes. The plan identity includes
this fixed policy version; it is not a new configuration toggle.

Stationary cells retain a full 64-bit last-seen accepted-frame ordinal and a saturating
16-bit-range streak. Both implementations prune expired cells before association,
then paint observations in detector order and row-major 9-by-3 neighborhood
order before birth admission. The inclusive maximum-Doppler rule applies to
learning, birth veto and quarantine; zero includes every Doppler. Storage
pressure limits observed occupied cells, not map size or established tracks.
The resident ABI supports the default 65536 occupied-cell limit, including maps
larger than that limit; a different requested limit explicitly refuses resident
tracker translation and remains available to host tracking. No silent retuning
or backend substitution is permitted. Duplicate/stale timestamps or consumed
sequences are no-ops even with zero retained tracks; the first timestamp may be
zero. Only committed accepted frames advance persistence age. Resident occupied
counts and cells use the same two-bank transaction as kinematics: prepare copies
committed state, abort/rejection discards tentative changes, and commit promotes
both banks together. Storage is 24 bytes per map cell plus 56 bytes when enabled.
Reset clears occupancy. Because the public kinematic checkpoint omits occupancy
and its clock, every stationary mode (including depth 1) admits only its exact
current warm checkpoint. Stale, foreign and cross-reset restores are refused;
current restore preserves occupancy and the accepted-frame clock.

`testGpuTrackerCorrectness --policy-parity` executes T01--T06 through actual
CPU Vulkan shaders and compares host checkpoints/counters where applicable.
It also seeds full-width rollover metadata directly. This verifies mechanics,
not RF sensitivity or false-track rates. Applying the retained comb defenses
to host tracking and changing boundary eligibility require paired whole-pass
positive/null and prospective evidence before promotion. Ordered resident
painting and its larger full-width buffer also require cadence/resource receipts.

## Explicit settings, not replay diagnostics

Three previously replay-gated settings now have identical live/replay admission.
Only transport predicates were removed; defaults, equations, precision and
physical/backend restrictions are unchanged:

- Target-protected array reference: explicitly selected fixed beam-bank direction,
  confirmed geometry, single lane, CPU or portable GPU. Steering derives only from
  configured bank index, frequency, surveyed positions and direct-path direction.
  No ADS-B identity/position/association or injected-target metadata enters it.
- Experimental FM reference quality filtering: explicit setting, enabled FM lane
  in channelized processing; current reference-IQ sample masking only. A waveform
  label never enables it. Existing resident reference-compatibility rules remain.
- Experimental `fixed_site_projected` bearing: retain five-channel array roles,
  GPU-resident clutter, ideal covariance bearing, coherent fusion, zero direct
  null and no measured bearing manifold. Live/replay state or container format
  no longer gates this configured model.

ATSC reference resolution already ignores live/replay state: dedicated ATSC 1.0
automatic mode uses the canonical pilot-aided reference, arrays remain raw, and
unsupported decision-directed/array-pilot combinations refuse. No ATSC math was
changed. Measured manifold admission still uses live applied-gain identity or
authenticated recorded acquisition identity as appropriate; calibration math is
unchanged. Physical confirmations survive solver-only edits but invalidate for
receiver identity, channel-role or measured-geometry changes.

## Evidence boundaries and regression consumers

Ordinary runners hash the admitted process block and check both in-memory and
written configuration after execution. Native immutable plan tests compare
live/replay identity across CPU, portable, resident and auto policy resolution.
The direct/channelized product regression also compares live/replay maps,
detections, tracks, sample intervals, temporal support, disabled stages and
discontinuity recovery; explicit FM filtering has paired product coverage.
API/UI tests cover identical admission and unsupported-route refusal, exact
recorded process loading, and positive/negative physical-confirmation persistence.
`test_canonical_processing_architecture.py` rejects hidden replay process writes,
alternate owners and public/live fixed-clutter selection. Native test execution
and exact-package hardware/recorded-IQ acceptance remain separate requirements.
Resident snapshot enablement cannot select resident versus host products;
noncoherent host-product snapshot export explicitly refuses instead of switching
the normal processing route. API capability tests also compare observer on/off.

Standalone snapshot tools (`WienerHopfReplayInspect`,
`DirectPathCoherentReplayInspect`, `TemporalEndpointReplay`,
`ResidentReplayInspect`) and manifold calibration/scoring tools are offline
analysis consumers, not ordinary IQ replay routes. Some deliberately compare
declared experimental parameters using canonical numerical owners (for example
Wiener loading arms); their diagnostic output is not live-parity or promotion
evidence. They must remain outside installed live entry points, retain pinned
input/provenance and negative controls, and cannot replace the production route.
