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
review, recorded-IQ/held-out PSF, reference stability, correlated
noise, full-search null calibration and radar-only geometry remain future work.
No live promotion is authorized.

Research basis: [Garry and Smith Part I](https://doi.org/10.1049/iet-rsn.2018.5233)
and [Part II](https://doi.org/10.1049/iet-rsn.2018.5236). Part II's experimental
ADS-B geometry is not used here and is not evidence of radar-only identifiability.
