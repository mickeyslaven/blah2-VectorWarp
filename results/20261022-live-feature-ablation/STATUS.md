# Live detection and positive-control fusion A/B — 2026-10-22

## Metric-integrity correction

The pass percentages below are retained only as provisional producer counters.
They are not an operational catch-rate estimate and must not drive promotion or
rollback. Independent review found that the producer does not yet guarantee a
configuration-independent eligibility denominator, CPI-event-time truth join,
measurement-supported stable-track criterion, complete truth audit, or nuisance
ledger. See `docs/OPRS_CRITICAL_REVIEW.md`.

## Earlier provisional live snapshot

At the 15:43 EDT snapshot, 72 finalized geometry-eligible passes had usable truth:

- 13 were authoritative tracked passes (18.1%).
- 59 were classified `no_radar_return` (81.9%).
- No pass met the `detected_not_tracked` criterion of at least three associated
  detection frames but fewer than three associated track frames.
- Three nominal misses contained only sub-threshold glimpses: EDV4640 had one
  detection frame; PITT89 had one detection and one track frame; N855ME had one
  detection and two track frames.

The associated examples prove that the live node detects and tracks real
aircraft, but the producer limitations prevent using this cohort to estimate
the catch rate or assign the dominant loss stage.

Elevation is a material separator in this limited live cohort: 13 of 54 passes
below 10 degrees elevation were tracked, versus 0 of 18 at 10 degrees or above.
This is observational evidence, not yet a causal claim; range, bistatic geometry,
and traffic class are confounded.

## Positive-controlled fusion replay

The same three exact-provenance positive clips (two independent passes) were
replayed through production coherent fusion and noise-whitened fusion. Both
arms retained a target detection and target track in every clip.

| Arm | Target-associated detection frames | All detection cells | Clips tracked |
|---|---:|---:|---:|
| Coherent beam bank | 11 | 1,792 | 3/3 |
| Noise-whitened beam bank | 15 | 1,913 | 3/3 |

Whitening increased truth-associated positive frames by 4/11 (36%) while
increasing the overall cell burden by 121/1,792 (6.8%). That is a promising
positive-control result, but only two independent passes are represented, so it
does not justify a live promotion by itself. It must next be evaluated on valid
misses or fresh live shadow data with pass-disjoint controls and false-alarm
burden measured.

Machine-readable detail is in `positive-fusion-ab-analysis.json`.

## Long stationary-clutter A/B result

The 7,099,688,272-byte replay was staged on Strix with source/target SHA-256
`c9513f8f48bf461ff0e00e3b66e8043c3d16f216bb708911e7d5112c620d1c59`.
The prior cached and copied deployed binaries rejected frame 2 with tracker
invariant code 316. A clean native build on Strix completed all 147 frames in
both matched GPU-resident arms without weakening validation. This localizes the
old rejection to the stale/copied replay build path rather than the recording.

The stationary-clutter filter suppressed 1 of 1,048 otherwise admitted
unmatched initiations (0.095%). ON and OFF produced the same 3,028 detector
cells, 3,553 published track-frame instances, 450 unique published track IDs,
and peak population of 183. Only the final internal population differed by one;
the number of published instances did not. This recording has no attached
exact ADS-B truth, so it is not an aircraft-recall estimate, but it rules out
this filter as a broad track-loss mechanism in the sampled workload.

Machine-readable results and the two full replay artifacts are in
`stationary-clutter-long-ab-summary.json`, `stationary-clutter-on-gpu.json`, and
`stationary-clutter-off-gpu.json`.
