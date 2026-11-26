# Offline passive-ISAR aperture foundation

This is a replay-only phase/lineage product and an unscaled residual-Doppler
signature, not a metre-scaled aircraft image, silhouette, classifier, 3D
reconstruction, radar improvement claim, or live processing lane. No truth,
ADS-B, tracker, acquisition, GPU worker or publisher enters its numerical API.

## Canonical ownership

`ExperimentReader::aperture` reuses the sealed snapshot decoder, channel-map
ordering and raw-anchor pilot estimator. It returns `shared_ptr<const
ApertureFrame>` before any temporal sum or cell/path winner. The export owns
all original per-channel complex CAFs and optional range/slow-time arrays
computed by `RangeMigrationReplay`. Opt-in retention does not change the
existing transform, detector banks or default runtime. Every declared rate,
including zero, is retained independently with its interpolation kernels.

Stored samples retain the canonical gauge: forward slow-time DFT unnormalized,
inverse divided by row count, rectangular block support, positive Doppler for
positive complex exponential, and positive excess sample delay. Excess path
uses c/fs, not c/(2fs). Migration delay refers to the midpoint of the first and
last correlation blocks. The recorded pilot correction is NOT already applied
to exported arrays. The rate interpolator accounts for the known reference
heterodyne range phase; the diagnostic removes that ramp and restores physical
block-center phase once, then removes prescribed bulk Doppler.

`PassiveIsarAperture` owns exact unique correlation-block support. First
observation wins overlapping blocks. The raw intervals, newly owned raw span,
block intervals and owned-row mask all remain explicit. Unused tail samples
are not extra profiles. Raw-contiguous CPIs may have explicitly declared
unprocessed tails shorter than the correlation-row count; the exact missing
support is retained and the diagnostic uses nonuniform absolute block times.
No padding or resampling fills a tail. Acquisition gaps, nonaligned
partial-block overlaps, duplicate/reordered frames, changed plans or calibration
epochs are refused. Derived IQ slices additionally refuse all raw overlap;
native whole-block overlap retains first-observation ownership.
Pilot failure cannot fall back to a power-derived phase.

## Export and bounded diagnostic

`passiveIsarApertureInspect` emits JSONL frame records followed by one `complete`
record. Only successful completion authorizes treating the stream as a complete
export. Exact sample/identity integers are decimal strings. Receipts include
executable/snapshot/sidecar/config/source/RF/module/device/raw-IQ identities,
all twenty source-block hashes, the full canonical configuration wire bytes,
per-channel axes/CAF/profiles, spatial coefficients, pilot/gauge, correlation
support, common masks, kernel weights, noise gains and trial/work counts.
Source snapshots remain necessary for cancellation/reference-state replay;
their hashes are provenance, not substitutes for retained original bytes.

The common immutable `ApertureInput` boundary accepts either a validated native
snapshot or authenticated derived IQ products, before coherent 2/4/8 summation,
fallback or winner selection. `loadAuthenticatedIqSegment` belongs to the
existing replay library. It authenticates the frozen development extraction
plan, receipt and selected payload, reuses the canonical sealed sidecar decoder,
checks exact parent raw/reference/filtered byte ranges, and admits calibration
segments only. The CLI computes their CAF with canonical `Ambiguity`, estimates
the same raw-anchor pilot, then calls the same aperture factory and migration
owner. It does not create smaller replacement snapshots or consume downstream
coherent phase-bank winners. The development receipt supplies the parent-file
authentication; this bounded consumer does not re-read entire parent IQ files.

Derived exports retain original parent identity/ranges separately from actual
slice identity/ranges, plus the complete pinned plan/receipt, payload hash and
parent byte spans. Missing native config wire bytes and physical carrier are
explicit nulls, not manufactured defaults; nonzero translation is refused when
the physical carrier is unknown. The extraction declares physical channel 0;
no full-array or manifold calibration is inferred from that declaration.

Effective occupied bandwidth and external-manifold qualification are explicitly
unknown. Reference-conditioning bandwidth is exported separately and must not
be interpreted as effective RF bandwidth. Carrier comes from the canonical
lane configuration, not an invented sample-rate or tuning-frequency default.
The native sealed CAF presently supports zero Doppler center; separate
canonical-IQ numerical tests exercise nonzero-center transform preservation.

The CLI requires an explicit physical single-channel projection and emits
every prescribed rate's spectrum. Constant translation obeys
fD=-carrier*delay_rate/sample_rate. The requested delay is at the first CPI's
block midpoint; subsequent midpoint coordinates follow that prescribed rate.
The same Lanczos owner samples profiles. Its prepared complex kernel is shared
with the coherent-motion experiment (`ab2f258945e3efe15ba86ee1d426a9d6cba81603`),
not copied into another interpolator. Preparation occurs once per frame/rate;
the 16 coefficients, finite support and fixed-cell white-noise gain are reused.
The helper preserves the heterodyne ramp at the sampled coordinate. A coherent
older-CPI consumer expressing that sample at the newest integer coordinate must
add exp(-i*2*pi*center*shift/fs) once per path/age. The aperture diagnostic instead
removes the ramp at its actual prescribed delay. Neither consumes a coherent
phase-bank winner, residual-Doppler maximum, power fallback or temporal sum.
Rectangular coherent sums are divided
by sqrt(unique_blocks). Output axes remain residual Doppler Hz: no aircraft
rotation, position or cross-range scale is inferred. This is not autofocus.
Within-block motion, reference/cancellation distortion, nonlinear motion,
scintillation and actual array calibration remain unresolved physical limits.

Admission limits are eight frames, five channels, nine explicit rates including
zero, 65536 CAF cells/channel, 1024 unique blocks, 128 MiB/snapshot and exported
frame bound, 256 MiB aggregate retained arrays, and 2^28 declared complex MACs.
Remaining aggregate transform budgets are passed before allocation/transform;
diagnostic admission charges all rates and requested bins. The process creates
no workers. Run it under a one-CPU, 2-GiB, bounded-time admitted job. Interpolation
white-noise gain is not covariance correction, independent exposure or CFAR.

## Acceptance status and next gate

Targets: `testPassiveIsarAperture`, `passiveIsarApertureInspect`; CTest:
`passiveIsarApertureCpuContract`. The independent Fourier-synthesized continuous
IQ test passes through canonical `Ambiguity` and sealed snapshots. Direct
time-domain correlation, not the candidate interpolator, provides the complex
block oracle; independent coherent Fourier sums check 2/4/8-frame signatures
and point/two-scatterer PSFs. Tests cover reference gauges, counters above 2^53,
overlap deduplication, nonzero-center numerical transforms, interpolation gain,
missing support, aliasing, order/gaps and pilot refusal.

Native acceptance on 2026-11-26 used the admitted Display2 host, unique path
`/var/tmp/vectorwarp-passive-isar-native.b2r8R3`, and image
`localhost/vw-fedora-builder-all:20261119`, with no network, one CPU, 2 GiB RAM,
2 GiB total memory+swap, jobs=1, and serial range/BLAS execution. Production CMake
target definitions were included by a small isolated native harness. Release
build used C++17, `-Wall -Werror`; the GPU runner remained inactive and disabled.
No radar service was changed. Results:

- `passiveIsarApertureCpuContract`: PASS, 3.34 seconds.
- `rangeMigrationReplayCpuContract`: PASS, 0.86 seconds.
- `passiveIsarApertureCliContract`: PASS, 4.03 seconds; real binary export,
  executable identity, exact counters/owned blocks, complete rate/channel arrays,
  independent complex oracle, and invalid request refusal before any output.
- Architecture/budget suite: 86 tests passed. One append-only, reviewed-purpose
  budget adjustment accounts for the offline aperture owner/export and shared
  helper; no blanket baseline refresh or weakened checker.

The continuous-IQ CPU contract uses FP64 transforms, then deliberately seals CAF
as FP32. Its inverse block correlation tolerance is 1e-10 before sealing, 2e-6
after sealing; the independent 2/4/8-frame spectrum tolerance is 3e-6, relative
to max(1, oracle magnitude). The six exported synthetic spectra had maximum
normalized complex error 6.50e-7. The shared sampler separately checks scalar
complex phase at 2e-12, finite-Lanczos analytic-tone error below .004, and noise
gain at 2e-15; this is a finite-band approximation, not ideal reconstruction.

Verified synthetic evidence and rendered illustration are in
`/var/tmp/vectorwarp-passive-isar-evidence.YBU5No/` (`synthetic-psf.jsonl`, `.svg`,
`.png`). The illustration uses measured test output only, shows 25/41/73 unique
blocks for overlapping 2/4/8-frame apertures, and normalizes each trace to its
own peak; it is not an SNR-gain or aircraft-imaging claim. Regenerate with
`TestPassiveIsarAperture --evidence-dir NEW_DIRECTORY` and
`test/unit/process/analysis/RenderPassiveIsarPsf.py INPUT.jsonl OUTPUT.svg`.
The renderer refuses absent or failed complex-oracle evidence.

Evidence SHA-256:
`827db62f496cf2633ffc188401ddecbb9705a75520944bec8fb94b0deee43cc4`.
CLI SHA-256:
`66f23f46a4141deffd6419931262c072b80cfd011d35b5406b84c727257d5dca`.
Native fixture/oracle executable SHA-256:
`f91031781804de24630db5b8b743bd66ca5dfcd9c3e4ead3918fe3d1c27d03d0`.

This qualifies the bounded synthetic CPU/export contract, not aircraft image
formation. The end-to-end PSFs have very small physical range walk; substantial
inter-CPI walk and full nonzero-center aperture formation still need separate
continuous-IQ acceptance beyond the shared kernel and transform tests. Independent
review, held-out PSF, reference stability, correlated
noise, full-search null calibration and radar-only geometry remain future work.
No live promotion is authorized.

## Bounded recorded-IQ contract qualification

The frozen calibration-only plan is
`/var/tmp/vectorwarp-passive-isar-recorded-20261126/plan.json`, SHA-256
`3fa584711a010dd0305c34c47a362639d8481d89eb3304bf5bc914ede04f3cd1`.
It uses only authenticated calibration segments 001–008 (original parent frames
1–2); no holdout, ADS-B or target labels are read. The fixed coordinate is delay
32 samples on extracted channel 0, zero prescribed translation, and residual
Doppler −60 through +60 Hz in one-Hz steps. These choices precede inspecting the
output. The parent snapshots exceed this foundation's native admission bound;
the existing extraction receipt, not fabricated reduced snapshots, provides the
bounded ingress.

Input plan SHA-256:
`3ebd4bd82902b68c041deee69d665219129a0ab5e6639ec603b63fa952b97c90`.
Extraction receipt SHA-256:
`d2896dd9bce401acb7ba1fb4d1a0161ae0df3f745bce2cd9ae3e0112260c10b0`.

Each CPI has 240000 acquired samples, 121 correlation blocks of 1983 samples,
and 57 unprocessed tail samples. The 2/4/8-CPI apertures retain respectively
242/484/968 unique blocks, 114/228/456 total tails, and 57/171/399 internal missing
samples. The last tail is unused support, not an internal gap. No tail becomes
an independent block or increases coherent normalization.

`QualifyRecordedPassiveIsar.py` independently inverts the retained CAF at every
used block and computes the nonuniform-time complex Fourier sums. This checks
export algebra/provenance, not physical aircraft scattering or target gain.
`RenderRecordedPassiveIsar.py` plots only PASS evidence and labels each curve's
own-peak normalization explicitly; it refuses imaging/promotion claims. Neither
routine identifies a target, chooses a peak, calibrates false alarms, estimates
geometry, or infers a metre-scaled image. Physical carrier, occupied bandwidth
and manifold qualification remain unknown for this derived extraction.

Exact-source rerun on the same isolated Display2 environment passed:

- Native aperture/continuous-IQ/tail contract: 4.51 seconds.
- Real CLI, native+derived round trip and adversarial refusal: 6.13 seconds.
- Existing direct coherent replay regression: 0.67 seconds.
- Canonical architecture/budget suite: 86 tests; 947 measured ceilings.
- Recorded 2/4/8 export: 1.12/2.13/4.44 seconds, all algebra/provenance checks
  passed. Maximum complex error was 2.74e-15, minimum pilot coherence 0.861.
  These offline wall times are not CPI deadline qualification.

Final binary SHA-256:
`16cefbb51373bba6c7bbac519e1d6f2c6e652e6779fe8815e079dfa2a937e376`.
Qualification JSON SHA-256:
`0f5dba56394a9869871fd06518e46eae0cecbdff1699ca4f105784860dd09502`.
Full JSONL exports and run receipt are on Display2 under
`/var/tmp/vectorwarp-passive-isar-native.b2r8R3/build/recorded-v2/`.
Local qualification, receipt and browser-rendered illustration are under
`/var/tmp/vectorwarp-passive-isar-recorded-20261126/`:
`qualification-v2.json`, `run-receipt-v2.json`, `recorded-aperture-v2.svg` and
`recorded-aperture-v2.png`. The 1100×690 screenshot was visually checked without
clipping; four failed/overclaiming/miscounted evidence mutations were refused by
the renderer before output. No image-generation model supplies its data.

The typed extension uses one additional append-only budget adjustment with
explicit reuse/rejection/cleanup rationale; it does not refresh the sealed
baseline. Independent review is still required before consolidation with the
coherent branch. No deployment or operational promotion is authorized.

Research basis: [Garry and Smith Part I](https://doi.org/10.1049/iet-rsn.2018.5233)
and [Part II](https://doi.org/10.1049/iet-rsn.2018.5236). Part II's experimental
ADS-B geometry is not used here and is not evidence of radar-only identifiability.
