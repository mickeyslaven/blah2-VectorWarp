# Live installed-manifold qualification

Frozen from the 563 MHz live node after raw-IQ capture was disabled. The
lightweight manifold recorder remained active; no `.mchq` recording was needed.

## Corpus

- 114 replay-capable coherent samples
- 22 independent aircraft passes / 20 aircraft
- 563 MHz, 2.4 Msps, five channels
- configuration SHA `ea5ddd10b6a717716ad1f44ad069fc1cb4f63172f8041f5ba247fd6f1783ff15`
- source sample SHA `55517b3ee5c9f13a5411aeffae5cc0cf2059b927a909e52c04dbffd5a44233d2`

Coverage is not yet suitable for 360-degree deployment. Three of eight 45-degree
local-azimuth sectors have useful independent coverage and the sample-level
60-beam fit has a 222-degree unsupported gap. The missing local-azimuth region
is approximately 90–270 degrees (roughly true bearings 170–350 degrees under
the confirmed 80-degree CH0 orientation).

## Pass-disjoint result

`lopo-60.json` is the predeclared live 60-beam configuration evaluated by
leave-one-pass-out cross-validation. Twenty-one of 22 folds fit successfully;
one fold was rejected because removing it enlarged the unsupported gap to 264
degrees.

| Model | Median error | Within 15° | Within 30° | Within 45° |
|---|---:|---:|---:|---:|
| Ideal pentagon | 69.85° | 9.5% | 14.3% | 23.8% |
| Measured installed manifold | 29.44° | 19.0% | 52.4% | 71.4% |

This is a credible indication that the installed manifold fixes much of the
wrong-lobe problem in the observed region. It is not a production qualification:
the same corpus is strongly sector-biased, three held-out passes regress badly,
and no evidence exists across more than half the circle.

## Production acceptance remaining

- at least 40 independent passes (currently 22);
- at least 12 withheld passes (the built-in deterministic split currently has 2);
- at least six of eight independently covered sectors (currently 3);
- five passes and three aircraft in every counted sector;
- held-out evidence in at least four sectors;
- measured median error at most 45 degrees, material improvement over ideal,
  no within-45 regression, and no sector regression over eight degrees.

Do not promote the candidate until those gates pass. Continue passive collection
through the lightweight manifold recorder while the raw-IQ clip recorder remains
disabled.
