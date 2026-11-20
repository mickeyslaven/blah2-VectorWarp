# Frozen depth-4 deduplication rerun — 2026-11-14

Neither frozen suppression arm wins on the new development cohort. Both
reduce duplicate and unmatched ACTIVE output, but both lose measured truth
support and continuity. This is an observed tradeoff, not sufficient evidence
to choose or promote a policy. Only one episode reaches the nominal lifetime
floor; no old scored records were reused and no settings were tuned afterward.

## Preregistration and unseen snapshot

Before downloading or inspecting new truth, the two suppression geometries
were frozen in `docs/SHADOW_DEPTH4_DEDUP_PREREG_20261114.json`, SHA-256
`97130baef3d264b4066372c9a32d3b838e19d1fdc240a0736bec02e4ec996bd2`.

The three arms are:

1. Raw depth-4 candidates.
2. Immediate-neighbor local maxima: suppress a lower-priority original point
   within one native CAF delay bin and one base-CPI Doppler bin, inclusive.
3. Existing configured centroid geometry: strict half-widths of 19 delay bins
   and six Doppler bins. The delay width follows the production scaling
   `ceil(6 × 6.144 MHz / 2 MHz)`; Doppler retains the configured six-bin width.

Suppression compares against the complete original list, including points
that are themselves suppressed. Priority is decreasing SNR, increasing delay,
increasing Doppler, then original index. Retained points preserve their exact
values and support lineage; there is no interpolation, averaging or truth
input. Equal-SNR tie handling is deterministic. The configured arm matches
production centroid geometry and unequal-SNR local-max semantics, rather than
claiming the entire production detection pipeline.

New complete source snapshot:
`/var/tmp/shadow-depth4-dedup-20261114.IMCgFk/source-full.jsonl`, SHA-256
`4bd2c9f4fc41b6f59154e516ae559c0665c8472cf0b7745c78e8390d761cd21b`.
It contains 947 rows. The exact previous 279-row byte prefix was verified and
excluded. Seventeen further rows from already-scored generation 11 were also
excluded. The untouched cohort contains 651 qualified records across 42 new
generations, 305 depth-4 endpoints and approximately 695 seconds. No further
qualification failures occurred. The raw qualified cohort is preserved at
`results/untouched-qualified-raw.jsonl` under the new snapshot directory,
SHA-256 `97b31bb13bcfb275319d1b615e5acbe74971bbf1ab0155ccc1aa445fc7bd9054`.

This is a new chronological cohort, not a certified aircraft-pass-disjoint
holdout: aircraft can persist across the boundary. Eight aircraft contribute
eligible truth. Forty-four generation/gap episodes are replayed, of which 40
can nominally confirm a track and only one has at least 20 depth-4 endpoints
(80 base CPIs). Maximum episode span is 55.999 seconds. There are no certified
complete passes or adjudicated nuisance windows.

## Fixed policy and executed results

All arms use the identical production CPU tracker with birth SNR 7 dB,
M=4/N=6/delete=3, base CPI 0.499953 s, the fixed normalized gate, adaptive
acceleration and ridge birth filtering disabled. Every generation change or
missing depth-4 endpoint resets state. Earliest seed-to-confirmation is six
seconds; the four-window evidence horizon is eight seconds, N-window horizon
12 seconds and deletion after four misses eight seconds. All inference outputs
were generated before evaluating truth.

The 305 endpoints contain 406 eligible aircraft opportunities; 241 endpoints
have at least one eligible aircraft. Matching gates and extent are unchanged
from the preregistration: ±0.25 km, ±10 Hz, recorded searchable delay extent,
and absolute Doppler 15–600 Hz.

| Metric | Raw | Adjacent-cell maximum | Configured centroid maximum |
|---|---:|---:|---:|
| Retained candidates | 16,103 | 11,782 | 11,204 |
| Truth candidate hits / 406 | 80 | 79 | 73 |
| Candidate hit rate | 19.70% | 19.46% | 17.98% |
| Windows with a truth candidate hit | 80 | 79 | 73 |
| Measured ACTIVE truth endpoints / 406 | 9 | 5 | 7 |
| Duplicate truth-near ACTIVE track endpoints | 4 | 0 | 0 |
| Unmatched ACTIVE track endpoints, unlabeled | 69 | 29 | 25 |
| Missed eligible truth endpoints | 397 | 401 | 399 |
| Matched eligible truth fragments / 77 | 2 | 2 | 2 |
| Extra measured track segments within fragments | 1 | 0 | 2 |
| Longest measured ACTIVE truth continuity | 12.00 s | 6.00 s | 4.00 s |
| Mean fragment's longest measured continuity | 0.208 s | 0.130 s | 0.104 s |
| Median measured-seed-to-confirmation | 6.00 s | 6.00 s | 6.00 s |

Duplicate counts measure extra simultaneous ACTIVE tracks near a truth point.
Unmatched tracks remain unlabeled and cannot establish a false-track rate.
Continuity reports consecutive measured support, while fragmentation separately
counts changes of track identity or interruption. Confirmation medians include
all confirmed tracks, not only truth-matched tracks.

Aircraft-cluster paired bootstrap 95% intervals against raw:

| Arm | Candidate-hit difference | ACTIVE-truth difference |
|---|---:|---:|
| Adjacent-cell maximum | −0.246 pp [−0.455, 0] | −0.985 pp [−1.818, 0] |
| Configured centroid maximum | −1.724 pp [−3.182, 0] | −0.493 pp [−0.909, 0] |

These intervals describe eight aircraft fragments in a short development
cohort, not independent complete passes. The report also preserves generation
cluster intervals and per-generation candidate effects.

All arms have zero shifted candidate hits at +17 seconds (80 opportunities)
and +31 seconds (12 opportunities). The +17-second control excludes seven
overlaps with current truth. There is no +61-second opportunity within a
single generation, so that control is unavailable rather than a zero-event
measurement. Controls never wrap or cross generations and remain dependent
sensitivity checks. They are not independent nuisance evidence.

## Reproduction, tests and boundary

Code: `script/rerun-depth4-dedup.py`, with the frozen existing CPU adapter and
the shared qualification/scoring helpers. Eight combined tests pass, including
strict/inclusive geometry boundaries, base-CPI Doppler normalization,
non-greedy local-max semantics, deterministic ties, lineage preservation and
qualification failures. The raw replay exactly matches recorded live
track/ACTIVE/COASTING counts at all 302 endpoints before any collection gap.

Full report:
`/var/tmp/shadow-depth4-dedup-20261114.IMCgFk/results/report.json`, SHA-256
`e280f2c55ffec36b24dcdb9f2a322eb63e8e6cf41f6a495ea012a68dde04b75c`.
It hashes the three truth-free tracker inputs, three complete outputs, raw
qualified cohort, preregistration, script and shared helper. Script SHA-256:
`7dd7057c2cfd7eecc8baad242edc11c67584dde7fa78c17eab3ed2f31c429792`.

Exact bounded command from `/var/tmp/vectorwarp-config-preservation-b7c3b8b`:

```sh
ulimit -v 786432
ulimit -t 60
taskset -c 8 nice -n 19 timeout 120 python3 script/rerun-depth4-dedup.py --snapshot /var/tmp/shadow-depth4-dedup-20261114.IMCgFk/source-full.jsonl --preregistration docs/SHADOW_DEPTH4_DEDUP_PREREG_20261114.json --tracker /var/tmp/shadow-rerun-v9-20261114.ltKwbp/temporal-endpoint-replay --output-directory /var/tmp/shadow-depth4-dedup-20261114.IMCgFk/reproduction
python3 -m unittest discover -s test/script -p 'test_rerun_*.py' -v
```

The completed analysis took under one second, used no GPU, and stayed within
the existing one-core/768-MiB/60-CPU-second/120-wall-second limits. Available
RAM remained approximately 16 GiB; heavy work was not attempted. Qwen/Gufo,
services, configuration and live inference were untouched. No release, commit,
push, PR or promotion occurred.

Next evidence requirement is sufficient uninterrupted, unseen whole-pass
episodes plus independent nuisance exposure. Preserve these frozen suppression
arms if repeating that study; do not tune a new width on this cohort. Their
output reduction alone is not an acceptable benefit when measured support and
continuity decrease.
