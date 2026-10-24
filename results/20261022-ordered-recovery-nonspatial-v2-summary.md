# Fresh-positive ordered recovery calibration

Date: 2026-10-22

## Decision

The 68-trial manifold-independent matrix did **not** pass leave-one-aircraft-pass-out positive-control validation. No miss was released for recovery scoring. Earlier recovery claims that lack the same fresh-positive, exact-provenance, pass-held-out calibration are non-authoritative.

## Cohort and execution

- Six fresh exact-provenance positive clips from five aircraft passes.
- Arms A, B, C, D, and I; 68 predeclared ordered/parameter trials per clip.
- 408 raw-IQ trial evaluations on Strix.
- Familywise control quantile: `1 - 0.01/68`.
- One manifest join defect was found and fixed: two clips from one pass shared an event ID and had been aliased. Event ID plus recorder revision is now the join key.
- One reporting defect was found and fixed: negative HMM natural-log scores had been ratio-clamped. Margins now use the log-likelihood difference converted to dB.

## Held-out result

- Best all-positive trial: arm D, adaptive spectral clipping -> 12 dB HDR clipping -> unknown-noise GLRT -> 6-state TBD/HMM.
- Pass-macro mean corrected margin: **-0.583 dB**.
- Leave-one-pass-out median corrected margin: **-0.00005 dB**.
- Held-out passes above 0 dB: **0/5**.
- Held-out passes at or above +3 dB: **0/5**.
- Useful-discrimination gate: **failed**.

The near-zero margins in four folds are not detections: their ranks are poor and the HMM log-odds are numerically compressed near the maximum translated-control score. The fifth fold is -2.892 dB. A single arm-I trial ranked one clip 75th among its translated controls, but this did not generalize across passes and is only a candidate diagnostic.

## Consequence

Running the 588 misses through these families would manufacture rankings without proof that the methods recover known aircraft. The miss phase remains blocked. The next valid steps are to qualify the installed measured manifold for spatial arms E-H and to obtain more fresh positive passes while retaining this identical validation protocol.

## Artifacts

- Matrix: `20261022-ordered-recovery-nonspatial-v2.json`
- Frozen result: `20261022-ordered-recovery-nonspatial-v2-frozen.json`
- Full-family deferred matrix: `20261022-ordered-recovery-all-families-v2.json`
