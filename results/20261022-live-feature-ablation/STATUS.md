# Live detection and positive-control fusion A/B — 2026-10-22

## Live answer

At the 15:43 EDT snapshot, 72 finalized geometry-eligible passes had usable truth:

- 13 were authoritative tracked passes (18.1%).
- 59 were classified `no_radar_return` (81.9%).
- No pass met the `detected_not_tracked` criterion of at least three associated
  detection frames but fewer than three associated track frames.
- Three nominal misses contained only sub-threshold glimpses: EDV4640 had one
  detection frame; PITT89 had one detection and one track frame; N855ME had one
  detection and two track frames.

This proves the live node detects and tracks real aircraft. It also shows that
the dominant measured loss is before robust tracker formation: most eligible
passes do not produce three truth-associated range–Doppler detections. The
Retina publication filter is not the aggregate bottleneck in this snapshot.

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
