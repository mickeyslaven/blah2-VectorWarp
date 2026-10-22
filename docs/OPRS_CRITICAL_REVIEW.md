# OPRS first-pass critical review — rejected for operational acceptance

An independent reviewer audited commit `aeaf62e`, the live producer, synthetic
edge cases, and the saved trial result. The first-pass aircraft-pass scorer is
useful only as a description of producer counters. It must not support live
performance or configuration conclusions until the evidence contract is fixed.

## Blocking findings

1. `api/evidence-statistics.js` derives eligibility from the configuration's
   ambiguity limits and `detection.minDelay`; a candidate can therefore change
   its own denominator. Use a versioned detector-independent evaluation
   envelope and require identical contract hashes.
2. Current/cached ADS-B predictions can be joined to detection/tracker frames up
   to roughly two seconds away. Persist unique CPI IDs/timestamps and interpolate
   truth to each CPI event time before association and scoring.
3. Any associated tracker cell can increment `tracked_frames`; confirmed state,
   stable ID, measurement update, duration and bounded gaps are not required.
   Separate tentative, confirmed, measurement-supported, coasting, fragmented
   and switched tracks. The saved apparent success has zero raw-detection frames
   and only 3 tracked frames among 213 opportunities.
4. Truth history is truncated while counts continue over the whole pass, so the
   retained tail cannot qualify every opportunity or success. Preserve a CPI
   join or complete auditable aggregates. Audit disagreement among all fresh
   sources, not only accepted ones.
5. Nuisance burden is not joined. Missing nuisance evidence must machine-block
   acceptance. Record unassociated new tracks/hour, unassociated track-minutes,
   concurrent display burden, fragmentation, and false/ambiguous/unknown labels.
6. Common support currently means only cell occupancy. One candidate observation
   can carry most baseline weight. Publish per-arm cell counts, exclusions,
   weight concentration, effective sample size and block/aircraft-resampled
   uncertainty. Below-threshold support is unidentified, not preliminary.
7. Cross-epoch comparisons can select a baseline implicitly and double-count
   cumulative snapshots. Require explicit disjoint arm/block manifests,
   deduplicate CPI/pass identity, and prefer paired replay or predeclared
   interleaved live blocks.
8. Continuity and latency are conditioned on success. Misses must contribute
   zero to headline pass-macro continuity; successful-track continuity is a
   separate diagnostic. Use censored acquisition-time analysis and reject
   negative timestamps rather than clamping them.
9. Strict input validation is missing. Synthetic probes admitted missing IDs,
   duplicate passes/truth timestamps, and counts larger than opportunities.
   Require schema/version/site/config/receiver/pass/CPI identities and quarantine
   impossible evidence.
10. The saved snapshot is censored and its source snapshot was intentionally not
    committed due size. A predeclared closure/washout rule and immutable evidence
    manifest are required.

## Required scorecard structure

Use independent gates for evidence readiness, live/RF health, reference and
cancellation, detection, tracking, bearing/manifold, recoverability, and
real-time resources. Missing evidence is `NOT_QUALIFIED` or `INVALID`, never
zero or pass. Do not blend gates into a flattering average.

The full open-source design is in `PASSIVE_RADAR_SCORECARD.md`; the eventual
implementation must support two-channel reference/surveillance systems as well
as coherent arrays and must keep synthetic, real-background injection, and real
aircraft claims separate.
