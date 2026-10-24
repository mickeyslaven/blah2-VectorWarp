# Replay-only ADS-B sparse delay-Doppler prototype

Date: 2026-10-21

## Scope and implementation

`script/analyze-adsb-sparse-delay-doppler.py` operates only on serialized
five-channel complex CAF cutouts. It does not import, mutate, or invoke live RF
or configuration code.

For every frame it forms the fixed ADS-B corridor of delay-bin offsets
`[-4, +4]` and Doppler offsets `[-20, -10, 0, +10, +20] Hz`. It solves a
complex group-lasso model with common delay-Doppler support and independent
complex amplitudes in the five coherent channels. Unpenalized nuisance terms
cover a constant background, delay and Doppler slopes, their cross term, a
zero-delay direct-path atom, and a zero-Doppler clutter atom.

Every one of the 45 candidates is retained and ranked. There is no +3 dB gate.
Every translated control that can contain the complete corridor receives the
identical inversion, physical score, and 45-candidate ranking.

The physical multiplier compares each candidate's Doppler with

`-carrier_frequency / c * d(excess_path_range)/dt`.

The reported coupled score is the geometric mean sparse evidence across frames
times a Gaussian consistency weight. The default 40 Hz tolerance is explicit
and is not an acceptance threshold.

## Corrected historical truth

The CLI accepts either a direct trajectory document or one row selected from a
corrected historical manifest using `--event-id` and `--revision`. Manifest
mode reads `replay_trajectory`, not the older trigger-time point estimate, and
records the truth provenance in the output.

The four corrected 527a/527b manifests contain 175 capture rows: 22 positives
and 153 misses. Of these, 167 use interpolated position samples and eight retain
an explicit `event_prediction_fallback`. Completed complex waveform cutouts for
those rows were not locally available during this prototype run, so they were
not replaced with legacy or guessed truth.

## Replay result and misses

The available wide five-channel positive `r277` was evaluated as a smoke test.
Its best corridor candidate was delay offset 0 and Doppler offset +10 Hz. The
target path ranked 25th among itself and 84 translated controls; 24 controls
equalled or exceeded it. Its margin over the control median was +1.431 dB and
its empirical survival probability was 0.2941. The nominal ADS-B candidate
(0 bins, 0 Hz) ranked 18th of 45 candidates.

This is a candidate miss, not a recovery: the known positive did not separate
from controls. In addition, `r277` has a legacy constant
`event_prediction_fallback` trajectory, so its range-rate-derived Doppler is
zero and it is not suitable for accepting the physical coupling. The intended
next acceptance check is the full corrected 527a/527b positive/miss matrix once
their complex cutouts are present. Report every rank and empirical control
probability; do not introduce a +3 dB pre-filter.

Durable results are in
`results/20261021-adsb-sparse-delay-doppler-matrix-v1.json` and
`results/r277-adsb-sparse-delay-doppler-v1.json`.
