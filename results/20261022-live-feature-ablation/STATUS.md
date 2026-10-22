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

## Long stationary-clutter A/B status

The 7,099,688,272-byte replay was staged on Strix with source/target SHA-256
`c9513f8f48bf461ff0e00e3b66e8043c3d16f216bb708911e7d5112c620d1c59`.
The replay launcher now reproduces the production default-on configuration and
sets the explicit Strix GPU identity. Both the cached calibrated binary and the
exact deployed live binary deterministically reject replay frame 2 with GPU
tracker invariant code 316. Frame 1 completes and commits; frame 2 fails while
validating the prior track-history record. The live receiver recorded zero
`GPU_TRACKER_REJECT` events in the inspected hour, so this is currently a
Strix replay/backend blocker, not evidence of a live-node failure.

No stationary-clutter aircraft conclusion is valid until the replay completes.
The next acceptance check is to identify which code-316 history/spatial
invariant fails on Strix, repair it without weakening validation, and complete
matched ON/OFF runs using the same recording and deployed binary revision.
