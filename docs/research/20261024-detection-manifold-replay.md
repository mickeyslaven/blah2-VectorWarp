# Detection-manifold replay diagnostic (offline, default off)

## Status

`script/evaluate-detection-manifold-replay.py` is an offline evidence tool. It
is not linked from the processor, API, packaged service, live configuration, or
deployment. The CLI exits unless `--enable-offline-experiment` is supplied.
The frequency-resolved pre-CAF arm is explicitly experimental. Neither this
tool nor the API truth-selected covariance replay is a production qualification
lane.

## Frozen causal comparison

Every post-CAF arm consumes the identical complex channel vector and searches
the identical declared direction bank. The report compares:

- ideal conventional steering;
- an unsteered coherent channel sum;
- ideal steering with noise-covariance whitening;
- a pass-disjoint measured manifold;
- covariance-adaptive MVDR/LCMV, optionally nulling the recorded direct mode;
- LCMV with both recorded direct and reflection constraints.

Measured vectors are keyed by direction, RF/frequency identifier, and the full
per-channel hardware-gain vector. A key is supported only after the configured
minimum number of distinct positive training passes. An unsupported direction,
frequency, or gain combination is never interpolated: it falls back to the
ideal vector and emits `supported:false`, `fallback:"ideal"`, and a support
mask. Supported entries retain training-pass IDs, sample count, and the maximum
within-cell angular residual as an uncertainty diagnostic.

The experimental pre-CAF arm accepts already extracted frequency-bin channel
vectors and an explicit direction/frequency manifold. It coherently combines
only supported bins and reports `unsupported` when none exist. It does not
alter or approximate the production CAF. When matched frequency-resolved
translated controls are supplied, it emits the same raw rank/margin and
trial-corrected fields and charges direction-count times used-bin-count as its
nuisance trials. Without matched controls it is marked diagnostic-only.

## Scoring contract

Training and scoring pass IDs must be disjoint; overlap is a hard error. Whole
positive and miss passes use the same target/control path. Truth fields label
the pass outcome only: they do not select direction, weights, cells, a lobe, or
a nuisance hypothesis. ADS-B inference is therefore always false.

For each method the output retains target and control raw scores, target raw
rank, raw dB margin over the empirical control p99, empirical survival
probability, number of direction nuisance trials, frozen method-family trials,
and the conservative multiplicative trial correction. Any future promotion must be based
on pass-level positives and misses, not CAF cells or frames. A candidate fails
if it improves positives by sacrificing the frozen miss/false-alarm gate, loses
constraint null depth, requires an unsupported-sector extrapolation, or only
wins before nuisance/family correction.

The executable v1 regression summary requires unique pass IDs and configurable
minimum positive and miss pass counts (two each by default). The measured arm's
median positive margin must not trail ideal steering, and its worst miss margin
must not exceed the ideal arm's worst miss margin. These small defaults exist
for deterministic tests, not publication; a field protocol must freeze larger
counts before looking at outcomes. This summary is diagnostic and cannot set the
API campaign's `qualified` state.

## Production-equivalent replay boundary

Production detection searches the complete beam bank for every CUT and every
CFAR training cell, then runs CA-CFAR on that max-beam power surface. Selecting
one azimuth/elevation response from ADS-B truth and dividing its power by a
spatial covariance quadratic form is a different statistic with a different
noise scale. It is useful for mechanism exploration, but it must not be compared
to a production CFAR alpha as if it were the live decision path.

The API therefore keeps all such comparisons diagnostic-only and always reports
qualification blocked. It has a strict import boundary for
`vectorwarp-detection-manifold-production-replay-v1` receipts. Those receipts
must bind independent IQ and complete input-grid hashes, the exact positive
corpus and candidate, frozen configuration/frequency/sample-rate/actual gain,
and the baseline/candidate full-bank hashes. For the identical target and local
nuisance CUT/training grids, both arms must retain beam count, raw CUT power,
training mean, alpha, threshold, and decision. The importer recomputes the CFAR
accounting and rejects truth-selected steering or partial support. The
default-off `detectionManifoldGridReplay` executable is only a post-CAF
arithmetic diagnostic. It consumes caller-supplied channel vectors rather than
immutable raw IQ and emits `same_iq:false` / `production_equivalent:false`;
therefore the production wrapper always rejects it and it cannot create an
importable qualification receipt. It still exports every beam on the supplied
CUT/training grid and binds the exact aircraft bank plus separate direct anchor
using the cross-language `vectorwarp-detection-bank-ieee754-v1` binary hash.
A future allowlisted native exporter must itself open immutable raw IQ and run
the production reference, clutter, ambiguity, fusion and full-bank CA-CFAR
path, with exact sample-range provenance. Whitening, multi-CPI integration,
motion banks and TBD also remain unqualified until their covariance/temporal
state is represented. The candidate receipt key is the artifact's
`calibration.candidate_model_sha256`, computed over the deterministic fitted
candidate before that key and its evaluation block are attached.

## Input boundary and limitations

The v1 input is JSON with `training_rows`, `ideal_manifold`, and
`scored_passes`; complex numbers are `[real, imaginary]`. Each scored pass must
provide `target_post_caf`, translated `control_post_caf`, `noise_vectors`, and
the recorded gain vector. Direct/reflection vectors and frequency-resolved data
are optional. The tool does not extract IQ, form a CAF, infer aircraft state,
fit from misses, resolve ambiguous bearings, or prove live improvement.

Deterministic tests cover measured-manifold gain on the same IQ, preservation
of target response and direct/reflection nulls, unsupported-sector fallback,
frequency/group-delay mismatch, per-channel-gain isolation, pass-disjoint
positive/miss scoring, raw regression fields, and trial correction.
