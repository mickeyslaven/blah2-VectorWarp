# VectorWarp operational aircraft-pass scorecard

This document defines the real-aircraft mission-outcome component of the broader
[Open Passive Radar Scorecard](PASSIVE_RADAR_SCORECARD.md). It is not, by itself,
a complete passive-radar qualification.

> **Status: rejected for operational acceptance.** The first implementation at
> commit `aeaf62e` exposed useful counters but the current producer does not yet
> guarantee a fixed denominator, CPI-event-time joins, stable measurement-backed
> tracks, complete truth coverage, or nuisance accounting. See
> [OPRS_CRITICAL_REVIEW.md](OPRS_CRITICAL_REVIEW.md). Generated results are
> provisional diagnostics until those blockers are resolved.

## Decision metric

The headline metric is **aircraft-pass tracking recall at a fixed nuisance-track
burden**:

`passes with >=3 ADS-B-associated tracked CPIs / finalized truth-valid geometry-eligible passes`

The independent unit is an aircraft pass, not a CPI, detection cell, clip, or
track ID. Repeated frames from one easy aircraft therefore cannot dominate the
result. Every estimate carries a two-sided Wilson 95% interval.

This is a constrained metric, not a weighted score. A change succeeds only when
tracking recall improves without materially increasing nuisance tracks. Track
count alone is never evidence of improvement.

## Frozen admission contract

A pass is scored only when all of the following are true:

- the pass is finalized and has at least three geometry-eligible CPIs;
- all scored truth is projected to CPI event time;
- absolute raw ADS-B observation offset is at most 1.0 second;
- no nearest-sample/fallback truth is used;
- fresh-source disagreement is at most 0.5 km;
- receiver, site, and configuration epochs are recorded.

Pending and truth-unusable passes are visible but excluded from the denominator.
Epochs are never pooled. Eligibility is based on fixed physical geometry and
truth rules, not whether the detector happened to produce output.

## Required scorecard

1. **Primary tracking recall**: pass has at least three associated tracked CPIs.
2. **Raw detection recall**: pass has at least three associated raw-detection
   CPIs, whether or not a track formed.
3. **Detection-to-track conversion**: tracked passes divided by detected passes.
4. **Nuisance burden**: unassociated new track IDs per valid radar hour, with
   tracker initiation/admission and stationary-clutter suppression as supporting
   diagnostics. Compare rate ratios, not raw totals.
5. **Continuity**: median, computed per pass, of associated tracked CPIs divided
   by eligible CPIs.
6. **Latency**: median seconds from first eligible CPI to first associated raw
   detection and first associated track.
7. **Bearing quality** (separate from detection): pass-macro accuracy within
   15/30/45 degrees and median absolute angular error. ADS-B is scoring only.
8. **Recoverability** (offline only): validated, positive-calibrated,
   trial-corrected recoveries divided by live misses. Candidates do not count as
   operational catches.

Always break out delay (`<=30 km`, `30–60 km`, `>60 km`), elevation (`<5°`,
`5–15°`, `>=15°`), absolute Doppler, azimuth sector, altitude, and array. The
headline remains all admitted production-eligible traffic; the 30 km delay band
is the fixed core-envelope diagnostic and must not replace unfavorable traffic
in the denominator.

## Comparison and acceptance

Compare complete configuration epochs using the same admission contract and
compatible uptime. Report both the unadjusted result and a baseline-weighted
delay/elevation stratified result once each arm has enough coverage.

Traffic and geometry are handled in three layers:

1. **Same-IQ paired replay** is preferred for DSP changes: both methods see the
   identical aircraft, geometry, interference, and noise realization.
2. **Interleaved live blocks** are preferred for a live decision: alternate
   baseline/candidate in short predeclared blocks over comparable times of day,
   without changing RF or gain. This prevents one arm from receiving only a
   favorable traffic period.
3. **Direct standardization** handles the remaining imbalance. Both epochs are
   scored within fixed joint delay/elevation cells, then reweighted to the
   baseline traffic distribution. Common-support coverage is reported; below
   80% coverage is not decision-grade. Doppler and azimuth are also reported as
   marginal breakdowns. With a large cohort, matching additionally uses
   circular azimuth, absolute Doppler, altitude, and time-of-day/direct-path
   condition.

Never “fix” changing traffic by dropping hard bins. Show raw recall, adjusted
recall, the cell weights, and excluded/common-support traffic together. A
configuration that only looks better because it saw closer or lower-elevation
aircraft has not improved.

- Fewer than 30 admitted independent passes per arm: descriptive only.
- At least 30 per arm: preliminary; report point differences and intervals.
- At least 100 per arm: decision-capable only if geometry coverage is adequate;
  statistical confidence, not sample count alone, decides the result.
- Promote a change only if tracking recall improves credibly, raw detection
  recall is not worse by more than 5 percentage points, and the nuisance-track
  rate is no more than 20% above baseline. Continuity and latency must have no
  material regression.

For live changes separated in time, uncertainty must be clustered by aircraft
pass (and by day for multi-day reports). Do not use frame-level significance.
Replay comparisons should use the same pass IQ and are paired by pass.

## Reproducible scorer

`script/score-operational-aircraft-passes.py` consumes one or more preserved
`evidence-statistics.json` snapshots, enforces the truth contract, separates
configuration epochs, and emits pass records, strata, Wilson intervals, latency,
and continuity. The live evidence file does not yet contain a trustworthy
unassociated-track-hour denominator, so nuisance burden must be joined from the
durable tracker diagnostic ledger rather than guessed.
