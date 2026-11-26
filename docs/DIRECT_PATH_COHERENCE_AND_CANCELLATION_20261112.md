# Offline direct-path coherent experiment and cancellation investigation

`src/process/integration/DirectPathCoherentExperiment.h` and its `ExperimentReader`
are consumed only by explicit offline replay tools and focused CPU tests. They do
not connect a new statistic to acquisition, CFAR, tracking, publication, or a
GPU dispatch. They reuse the existing `CoherentPathEvidence` integrator. The
release executable retains only the sealed replay-lineage sidecar serializer,
not the coherent experiment. Enable `BLAH2_BUILD_OFFLINE_TOOLS` explicitly (or
build tests) to build the replay consumers.
The next acceptance step is exact-IQ replay with recorded sample lineage and
pre-cancellation anchor IQ; old selected v1 cutouts are insufficient.

## Phase-preserving hybrid replay contract

The existing experiment also exposes `Result::hybridAt(depthIndex, cell)` for
2/4/8-CPI comparisons. A complete path passing the pilot and target coherence
gates retains its complex sum, normalized by `sqrt(depth)` and referenced to
the newest CPI. Otherwise the view selects the corresponding canonical
`TemporalEvidence` motion bank, retaining its exact FP64 mean power. Recorded
`currentPower` is admitted as power; it is neither squared again nor sent
through a square-root/norm round trip. No new detector or publication path is
created. `directPathCoherentReplayInspect` reports each family's selected-cell
counts, with `null_calibrated=0` and `detector_mutation=0`.

Both families use the canonical fractional-delay coordinate rule. Doppler phase
is compensated segment by segment using exact capture-sample hops, including
nonuniform Doppler axes. The sealed reader binds fallback beam tolerance and
physical delay drift from the recorded radar plan. One experiment instance
uses one immutable spatial coefficient vector. The optional selected-beam mode
of `CoherentPathEvidence` requires the same known beam on every contributing
CPI; an adjacent or unknown beam cannot establish phase continuity.
`CoherentBeamBank::process_coherent_into` supplies complex projected values and
selected beam identities; its accepted `process_into` projection remains real
amplitude with identical beam ranking.

Results retain newest-first source sequences, exact sample ranges, capture and
calibration epochs, rate and tuning, as well as the selected path's motion.
Pilot loss clears coherent history while valid power history continues. Capture
identity/hop changes restart both histories, and repeated or reordered source
CPIs are refused. The existing `ConfirmationLedger` rejects shared source CPIs
across depths, overlapping windows, and coherent/fallback selections. Scores
are not confirmations: neither this ledger nor hybrid scores enter tracking.

This is an uncalibrated offline experiment. Promotion still needs independent
null calibration of the joint pilot/phase gates, path and spatial searches,
depth selection and fallback mixture, plus disjoint whole-pass and prospective
evaluation of detections, measurement-backed continuity, false tracks, cadence
and capture health. Existing Gamma(L) thresholds cannot calibrate the coherent
or hybrid score. The regression suite checks mechanics and fallback parity;
it does not establish radar gain or false-alarm performance.

## Cancellation is the first experiment

The CPU implementation in `WienerHopf.cpp` forms reference autocorrelation and
surveillance/reference cross-correlation on the same CPI, constructs a Hermitian
Toeplitz matrix `R`, solves `R h = b`, and subtracts the full linear filtered
reference `L_x h`. The GPU native factor/solve/residual correction in
`GpuNativeRefinement.private.h` implements the same unregularized objective with
precision checks. Its residual correction is numerical refinement, not target
protection. `reference_synthesis.diagonal_loading` loads the array reference
covariance; it does not regularize this Wiener solve.

`followAmbiguity` sets cancellation support to the full ambiguity delay interval
(the filter's upper endpoint is exclusive). A delayed low-Doppler target inside
that reference subspace can be fitted and removed. A separate mechanism can
remove even nonzero-Doppler echoes: the reference is synthesized from the same
array channels. If `y_c = a_c s + b_c q` and `r = a_r s + b_r q`, cancellation
dominated by the direct term leaves approximately
`(b_c - a_c b_r/a_r) q`. A target with the same relative spatial response as the
direct path can disappear even with only a zero-lag tap. This is a plausible
mechanism, not a measurement that current aircraft were erased.

The selected post-clutter MXY/AAL cutouts cannot measure that loss. The existing
receipt reports negative eight-CPI contrasts, missing production weights and
sample-lineage receipts, and no post-warmup evaluation CPI; none proves a
cancellation gain or loss. Existing diagonal/fixed-.75/robust post-clutter
bootstrap and partial common-mode subtraction are different statistic families
or additional subtraction, not repairs for a signal already canceled.

Run one bounded sequential replay matrix over immutable raw recordings:

|Arm|Reference and cancellation change|Purpose|
|---|---|---|
|A|Exact baseline, full existing taps, no loading|Paired baseline|
|B|Same reference, cancellation off|Measure available echo and clutter before subtraction|
|C|Same reference, predeclared narrow tap interval around the direct path|Test cancellation support without target-selected exclusions|
|D/E|Same full support, `R + lambda * real(R[0,0]) I`, lambda=.001/.01|Bound fitting and improve conditioning; these are new replay-only cancellation arms|
|F, conditional|Existing replay-only fixed-bank target-protected reference with baseline cancellation|Disentangle reference contamination if A/B/C/D/E reveal attenuation|

For D/E, loading must affect every CPU prepared/unprepared solve and every GPU
factor, solve-residual check and correction consistently before a GPU comparison
is meaningful. Repeatedly solving the *unloaded* residual and adding correction
can undo regularization. Do the initial experiment with a portable CPU solve;
do not modify the live native precision-refinement loop speculatively.

Keep a frozen baseline synthesized reference and applied weights for A--E first
to isolate cancellation. Then rerun the selected arm end to end with independent
reset/warmup to assess changes from reference learning. Use paired injected and
uninjected samples at identical positions; fixed filter weights versus refitted
weights separate direct projection loss from adaptive reference/filter response.
Use a predeclared grid of delay, Doppler (including zero and both signs), spatial
separation and weak amplitudes. Existing `ReplayEchoInjector` is replay-only and
retains absolute Doppler phase, but lacks preceding-CPI delay history and uses
each channel's own recording as the delayed source. Respect/discard its leading
delay transient and call its output a controlled injection, not an exact
independent-channel propagation model. Injecting only *after* cancellation
cannot measure cancellation loss.

Report differential echo retention, target/local-background contrast, the
residual direct-path/clutter power, solve rejection and conditioning, runtime,
and downstream detection/whole-pass performance separately. Calibration and
evaluation recordings/passes must be disjoint. Apply identical search/trials to
controls; evaluate at the same empirical false-alarm/false-track exposure, not
equal raw thresholds or raised target peaks. Zero observed false events is not
proof of a tiny PFA. If reference support/weights, warmup or adjudicated negative
exposure is missing, retain diagnostic scores and `PFA/false_track_delta=null`.
No arm is promoted on a selected cutout or injected target alone.

## Exact replay result and rejected arm

The sealed HP replay at
`/var/tmp/vectorwarp-hp-replay-strix-20261112-lineage-r2` contains 20 complete
resident snapshots and 20 exclusive lineage sidecars. Missing, substituted,
reordered, and provenance-changing sidecars are rejected. The reader copies
the snapshot's validated `currentPower` block as its operational fallback; it
does not manufacture an all-zero fallback. Cross-frame continuity binds the
capture/configuration/module/device/source/RF identities, applied gains,
tuning, sample rate, and gain-calibration epoch.

The zero-motion coherent diagnostic warmed all expected 2/4/8-CPI banks, but
its accepted-cell fractions closely match the elementary isotropic-noise
coherence probabilities at the configured 0.6 threshold: approximately 40%,
6.4%, and 0.164%. This is not a calibrated null model for correlated radar
maps, but it provides no target-gain evidence. The scores remain offline-only,
with `null_calibrated=0` and `detector_mutation=0`.

Arm C is rejected. Relative to the baseline it raised detections from 64 to 95
and associations from 17 to 54, but 78/95 detections occupied one nearly
stationary delay locus and six repeated Doppler lines near +/-28, +/-42, and
+/-56 Hz. Neither arm confirmed a track. The improved measured fraction was
persistent comb/nuisance energy, not recovered target continuity. Range-comb
admission safeguards therefore remain unchanged. Arms D/E (relative diagonal
loading 0.001/0.01 with full support) are the next bounded cancellation
experiment; the CPU implementation and zero-default contract are in place,
but no live or GPU connection is authorized by this result.

## Bounded D/E CPU cancellation replay contract

`wienerHopfReplayInspect` now preflights a maximum of 20 ordered sealed frames,
with at most 2 million samples, 1,024 full-support taps and eight channels per
frame. It checks exact CPI sample intervals, fixed sample hop, consecutive
logical/generation/consumed sequences and unchanged capture/configuration/module/
device/source/RF/gain identities. A canonical per-frame digest binds every
decoded sidecar field, its gain vector, and the snapshot digest. The ordered
cohort digest and second-pass identity checks reject snapshot **or sidecar**
replacement after validation. Only one snapshot payload is retained at a time.

The explicit `--diagnostic-only` mode evaluates A/D/E sequentially using the
same recorded reference IQ and applied reference checkpoint. Each frame/arm
prepares one loaded covariance for all channels; each surveillance input starts
from immutable raw IQ. A repeated fresh CPU A control must reproduce exactly.
The report binds the executable digest, frozen input/cohort/reference/checkpoint
digests and original full cancellation interval. No reference learning, GPU
refinement, detector or tracker is executed by this runner.

The JSONL measurements report total residual power, change from paired A,
solve rejection, preparation/filter timing, native-baseline error normalized by
raw power, and a Cholesky diagonal ratio. The latter is explicitly a factor
pivot-spread proxy, **not** a reciprocal condition number. Total residual power
is not isolated direct-path/clutter power or target retention. Loading can
retain a target component and can also retain nuisance energy; these require
separate measurements. Rejected solves remain visible and invalidate their
paired comparisons.

Without `--diagnostic-only`, the runner returns exit 3 with an exact missing-
evidence report after validating the cohort. The retained data do not supply:

- A predeclared paired injected/uninjected grid with fixed-versus-refitted filter
  controls and independent target/local-background regions.
- A selected-arm end-to-end replay with independent reference reset/warmup.
- Disjoint calibration/evaluation whole-pass labels and adjudicated negative
  exposure, with paired downstream evaluation at equal empirical false alarms.

Accordingly echo retention, target/background contrast, PFA, false-track delta,
whole-pass performance and track continuity remain `null`; successful numerical
diagnostics do not complete the promotion experiment. Missing, reordered,
duplicate, changed-provenance or replaced-lineage inputs are hard failures.

Focused CPU tests cover zero-default preservation, singular-covariance loading,
the analytic one-tap regularized solution, both D/E strengths, prepared versus
unprepared paths, full versus blocked FFT/FIR execution (including a partial
last block), float versus widened-double inputs, frozen reference preservation,
portable executable-path resolution and changed sidecar lineage after freeze.

The completed bounded replay used all 20 sealed frames, five channels per frame,
1,200,000 samples per channel and the unchanged `[-10,500)` (510-tap) support.
All 300 A/D/E measurements completed with zero solve rejections; all 100 repeated
CPU A controls reproduced exactly. CPU A versus saved native filtered IQ had
mean squared error/raw power from `1.2521e-15` to `1.3150e-15`.

|Arm|Mean paired residual-power ratio to A|Pairwise range|Mean solve/filter time per channel|
|---|---:|---:|---:|
|A, lambda 0|1|1|37.53 ms|
|D, lambda .001|1.000962|1.000110–1.003461|37.67 ms|
|E, lambda .01|1.092304|1.010556–1.331268|37.71 ms|

These are arithmetic means of paired frame/channel ratios, not pooled power
ratios. Neither loaded arm reduced total residual power on this cohort. That
does not determine target retention: extra residual energy may be target,
clutter or noise. Factor diagonal ratios increased slightly (A mean .956510,
D .956661, E .957981), without establishing a condition-number estimate or a
target/track benefit. D/E therefore remain unselected diagnostics.

The final report is
`/var/tmp/vectorwarp-config-preservation-b7c3b8b/bin/cancellation-replay-20261112.jsonl`,
with the aggregate `bin/cancellation-replay-20261112-summary.json`, executable
`bin/wienerHopfReplayInspectOptimized`, artifact digests in
`bin/cancellation-replay-20261112-artifacts.sha256` and resolved library list in
`bin/cancellation-replay-20261112-libraries.txt` under that same worktree.
Ordered snapshot-plus-lineage cohort SHA-256:
`09618a2503e547044d19fdae7d1c323bfc0d6d7cdcd8d8de5345935a2035484a`.
The run used CPU 12, one BLAS/FFTW thread, a 4-GiB virtual-memory ceiling and a
600-second timeout; no GPU execution or service mutation occurred. A development
run stopped before the stronger lineage binding was retained separately as
`bin/cancellation-replay-20261112-aborted-before-lineage-bind.jsonl` and is not
an acceptance receipt. Missing-file, missing-evaluation and reversed-cohort CLI
checks returned their expected nonzero codes, in addition to the passing CPU
numerical/lineage tests.

Executable provenance was subsequently hardened to read the running executable
through `/proc/self/exe` on Linux or `_NSGetExecutablePath` on macOS. The POSIX
PATH fallback now skips non-executable same-name files, with a regression for
that shadowing case. The completed replay remains valid: it was invoked using
the explicit `bin/wienerHopfReplayInspectOptimized` path, and its recorded
executable SHA-256 still matches that preserved binary:
`be0a3db7bbf398fb8a95048ca2cbb49e05d07dc5d2730092f1956f2d536d07db`.
The hardened runner is rebuilt separately as `bin/wienerHopfReplayInspect`;
the retained numerical receipt is not relabeled as a run of that newer binary.

### Track-continuity audit

The canonical tracker associates measured delay/Doppler observations, expires
tentative hypotheses within its configured confirmation horizon and retires
sufficiently inactive tracks. Host and resident execution share Doppler/range-comb
and nuisance admission rules; ordinary confirmation counts measured hits, while the
experimental lineage mode separately credits disjoint support windows and never
credits prediction alone. A cancellation residual-power improvement cannot
establish any of those downstream outcomes. The earlier C arm's repeated comb
energy demonstrates why increased association counts are insufficient.

No tracker gate, M/N rule, coasting lifetime, comb guard or live detector setting
was changed by D/E work. The eventual whole-pass comparison must report real
measured support, independent confirmation credits, confirmation latency,
fragmentation, coasting/misses, false tracks per negative hour and nuisance
admission by arm under the same frozen policy. The retained 20 half-second CPIs
provide only ten seconds of diagnostic exposure, not a whole-pass or false-track
denominator.

## Portable coherent connection now present

`DirectPathCoherentShadow` accepts complex channel CAFs, a fixed channel ordering,
one immutable unit-norm spatial coefficient vector, a physically fixed raw
anchor, and the already computed power-map fallback. Its fixed beam voltage is
`z_k = sum_c a_c CAF_c,k`. Different beams use separate instances and their full
bank size is declared as a trial count; per-CPI winning beams must not be spliced
into one complex trajectory.

The pilot is `p_k = sum(raw_anchor * conj(reference)) / sqrt(E_anchor E_ref)`.
It is measured before cancellation, from the exact same `[first,end)` sample
range, and its squared normalized correlation supplies the quality gate. The
wrapper removes only its phase: `z'_k = z_k * conj(p_k)/abs(p_k)`. It never divides
by pilot amplitude. A pilot from an independently changing physical channel,
post-cancellation map, target cell, or reference-weight proxy is not accepted.

The existing `blah2.cpp::direct_path_pilot` sums those channel correlations after
multiplying by `conj(reference_weight)`. For an eigenbeam `r=w^H x`, `R w≈lambda w`,
that sum is approximately real positive. Under `w→w exp(i phi)`, CAFs rotate but
the weighted pilot is invariant. It must not be presented as the fixed-anchor
phase measurement this wrapper requires. The old function remains unchanged.

For depth L=2/4/8 and an existing motion path, `CoherentPathEvidence` accumulates
older samples after a Doppler phase rotation
`exp(i sign * 2 pi * integral(f dt))`, integrating each segment trapezoidally
using exact first-sample separation/sample rate. Its output is `|sum z'|^2/L`
with measured coherence `|sum z'|^2/(L sum |z'|^2)`. The sign is predeclared;
it must be validated against actual CAF/replay injection convention. Channel
calibration, off-bin Doppler, finite-CPI phase origin and migration remain exact-IQ
qualification requirements. This adapter does not add fractional-delay complex
interpolation or claim correction of independent per-channel phase drift.

Every frame binds capture epoch, sequence, exact sample interval, sample rate,
tuning, gain/calibration epoch, fixed anchor and logical channel IDs. Pilot/map
identity mismatch, poor pilot quality, invalid IQ/map values or unavailable
sample positions resets history. Sequence/sample-hop or epoch changes start a
fresh cold window. Configuration/geometry/hop rejection cannot reuse the old
window. The accepted fallback map is borrowed as a const pointer and never
modified. `usePowerFallback(bank,cell)` remains true during warmup, invalid paths
and failed target-coherence gates. The adapter emits separate score families;
it does not compare their raw magnitudes to select an operational detection.

For one declared sign and one quality setting, the declared per-cell envelope
is `spatial_trials * depth_count * (2 delay_motion+1) * (2 doppler_motion+1)`.
Changing anchor, sign, quality threshold or regularization arm after seeing
results adds hypotheses. Map cells, overlapping windows, target/coherence
selection and repeated opportunities still need external null calibration.
Coherent Gaussian fixed-path power has shape one, not the noncoherent
Gamma(L) law in `TrackBeforeDetect.private.h`; neither its threshold nor the
existing beam correction transfers automatically. `nullCalibrated=false` is
deliberate. Track confirmation must retain disjoint-CPI credit accounting.

Default storage is bounded to 65,536 cells and eight complex combined maps
(about 8 MiB of retained voltages per spatial hypothesis). A declared larger
capacity is capped at 1,048,576 cells. Output maps and caller-owned channel maps
are additional storage. This CPU adapter is a replay scaffold, not a qualified
full-map real-time workload.

## Precise resident integration boundary still outstanding

The resident complex map is `VulkanBackend::doppler_`, `vec2`, indexed as
`channel*M + delay*dopplerCount + shiftedRow`, with
`shiftedRow=(logicalRow+dopplerCount/2+1)%dopplerCount`.
`GpuResidentFrame.private.h::coherentFusionSource` currently discards complex
voltage and stores `(sqrt(bestPower), bestBeam)` in `Workspace::fused`.
`GpuPostprocess.private.h::integration_`, `currentPower_` and `tbdBanks_` are
power-only. `replayBank` is beam power only. None is complex history.

`exportReplayBlocks()` can already export raw IQ, synthesized reference IQ,
current complex channel maps, actual fusion/refinement weights, and power
history. The current `resident::Frame` and replay Identity carry logical and
consumed sequence plus millisecond timestamp; they do not carry the exact
capture first/end-sample range this adapter requires. Supply that from a verified
capture sidecar or a versioned wire/snapshot extension; never infer it from
nominal CPI or host timestamps. Old v1 complex cutouts likewise lack this receipt.

A future shadow GPU stage belongs after `dopplerPlan_` and before CAF storage is
reused. It needs a pilot reduction before `canonical` raw IQ is overwritten by
subtraction, a combined-voltage ring `8*M*8` bytes per fixed FP32 spatial
hypothesis, a bounded lineage/quality ring, separate 2/4/8 score/validity outputs,
and an independent diagnostic status. Reuse map/weight buffers read-only;
never change `fused`, `tbdBanks`, detector metadata or tracker admission. Its
reset must join `appendInputReset`, budget must join resident allocation and
storage-buffer checks, and disabled mode must allocate/dispatch nothing.
Pilot failure should invalidate only shadow evidence, not the established
detector. A new shader must match this CPU oracle, including data layout,
reference-gauge behavior, reset/edge rules and statistical-family labels before
any backend is called qualified. A simultaneous full beam-history bank is not
the smallest first implementation.

Focused local validation on 2026-11-12: single CPU 12, C++17 `-O0 -g0`, no GPU
work or service mutation; seven new cases (559 assertions) plus the existing
temporal/bootstrap/null-calibration tests pass: 24 cases, 2,249 assertions.
Checks include fixed/moving paths, spatial plus temporal coherent gain,
reference-gauge invariance, exact lineage resets, quality fallback and
independent-noise normalization. Binary:
`/var/tmp/vectorwarp-direct-path-shadow-rCrI3c/testDirectPathCoherentShadow`.
