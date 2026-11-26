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
are not extra profiles. Nonaligned partial-block overlaps, raw/processed gaps,
duplicate/reordered frames, changed plans or calibration epochs are refused.
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
The same Lanczos owner samples profiles. Rectangular coherent sums are divided
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

At the initial checkpoint strict C++17 syntax checks passed. Native execution
is pending an admitted remote slot; do not treat authored assertions as passed.
The architecture suite passes 85 checks with its repository-budget gate failing
on explicit new owner/tool growth; ceilings were not silently refreshed.
Complete native/CLI acceptance and independent review before claiming this
foundation qualified. Recorded-IQ/held-out PSF, reference stability, correlated
noise, full-search null calibration and radar-only geometry remain future work.
No live promotion is authorized.

Research basis: [Garry and Smith Part I](https://doi.org/10.1049/iet-rsn.2018.5233)
and [Part II](https://doi.org/10.1049/iet-rsn.2018.5236). Part II's experimental
ADS-B geometry is not used here and is not evidence of radar-only identifiability.
