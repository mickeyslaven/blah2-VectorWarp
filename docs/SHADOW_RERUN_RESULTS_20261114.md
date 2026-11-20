# Executed shadow reruns — 2026-11-14

The repaired HomePortal temporal candidate lanes retain a development signal,
but their tracking behavior does not justify promotion. Depth 4 remains the
most useful next candidate. Depth 8 improves candidate availability but loses
all truth-matched ACTIVE support in the matched-policy replay. Depths 2/4
produce many duplicate tracks and more unmatched ACTIVE outputs.

## Frozen evidence and exclusions

Source copied from HomePortal's v9 ledger under
`/var/lib/vectorwarp-api/temporal-pairs/` (private basename omitted).
Frozen snapshot:
`/var/tmp/shadow-rerun-v9-20261114.ltKwbp/source.jsonl`.
SHA-256: `f7460445d3eaec29747592af1af6a37d8246e6827d27d58970cac6743e36f1dc`.

The snapshot has 279 records over approximately 314 seconds. Row zero belongs
to generation 18 and lacks the v9 tracker-policy fields; quarantine it despite
its collector-side v9 binary label. The accepted 278 records span generations
1–11, six eligible aircraft fragments, and 278/134/67 depth-2/4/8 windows.
All accepted windows have matching native and shadow endpoints, complete
bounded candidate exports, and contiguous candidate support lineage. Maximum
candidate counts are 116/135/158, below the 256 export limit. This is complete
export of the configured candidate algorithm, not a complete CAF surface.

Search extent is the recorded 0.780710–59.968250 km; Doppler eligibility is
15–600 Hz absolute. Evaluation gates are ±0.25 km and ±10 Hz. Truth is used
only after radar inference. These are candidate hit diagnostics, not a
whole-pass detector acceptance metric.

## Candidate rerun

The primary cross-depth comparison uses exactly the same 67 endpoints and
204 truth opportunities. Native hits are 43 for every depth.

| Depth | Shadow hits | Paired difference | 95% aircraft-cluster bootstrap interval |
|---|---:|---:|---:|
| 2 | 56 | +6.37 percentage points | 0.00 to 14.34 pp |
| 4 | 70 | +13.24 percentage points | 0.00 to 26.29 pp |
| 8 | 70 | +13.24 percentage points | 0.00 to 27.49 pp |

Using each depth's own exact native/shadow endpoints gives native→shadow
189→244/852, 88→135/410, and 43→70/204. The machine-readable report also
contains results by generation and generation-cluster intervals. Aircraft and
generation cluster resampling are sensitivity analyses on this short snapshot;
neither makes these complete independent passes. The aircraft intervals touch
zero, and there are only six aircraft fragments.

Three prespecified temporal offsets (+17/+31/+61 seconds) are evaluated
without wrapping or crossing generations. Shifted truth overlapping current
truth within twice the evaluation gate is excluded.

| Depth | +17 s shadow/control opportunities | +31 s | +61 s |
|---|---:|---:|---:|
| 2 | 4/496 | 2/315 | 0/140 |
| 4 | 1/246 | 0/158 | 0/70 |
| 8 | 0/122 | 0/77 | 0/35 |

These offsets share a ledger and are dependent sensitivity controls. They do
not constitute independent nuisance exposure or a false-track measurement.
The report includes native control rates and paired control effects.

## Six matched CPU tracking replays

Each depth was replayed twice, once with native candidates and once with
temporal candidates at identical endpoints. Both arms use the production CPU
Tracker/Track/Detection sources, M=4, N=6, delete=3, birth SNR=7 dB,
adaptive initial acceleration, the current fixed normalized circle, and the
base CPI (0.499953 s) for Doppler resolution. Both disable the ridge birth
filter, matching the shadow tracker's policy. Native display detections are
quantized; this is not an exact-IQ/device-parity comparison or the complete
live native policy. No production source or inference configuration changed.

State resets at every generation change and missing endpoint. Depths 2/4/8
have 14/12/10 resulting episodes; 12/10/5 have enough endpoints to confirm
even one track. Only 1/2/2 episodes meet the nominal 80-base-CPI duration
floor. None are certified complete passes. Update intervals are approximately
1/2/4 s. Minimum measured-seed-to-confirmation delays are 3/6/12 s;
four-window evidence horizons are 4/8/16 s, N-window horizons 6/12/24 s,
and deletion after four misses takes 4/8/16 s.

| Depth | ACTIVE truth endpoints native→shadow | Missed truth endpoints | Unmatched ACTIVE track endpoints | Duplicate ACTIVE truth-track endpoints |
|---|---:|---:|---:|---:|
| 2 | 118→160 / 852 | 734→692 | 155→767 | 0→441 |
| 4 | 35→43 / 410 | 375→367 | 31→177 | 0→76 |
| 8 | 15→0 / 204 | 189→204 | 8→50 | 0→0 |

Unmatched outputs remain unlabeled; they cannot be called false tracks.
Maximum measured ACTIVE continuity is 13→22 s, 12→30 s and 16→0 s.
Extra measured track segments within eligible truth fragments are 23→23,
13→3 and 4→0; the depth-8 zero results from recovering no fragment.
Median seed-to-confirmation delays over all emitted confirmed tracks are
3.001→3.000 s, 8→6 s and 17.9995→12.001 s. These are development summaries,
not whole-pass continuity or confirmation-delay acceptance results.

Only 100/244, 17/135 and 2/70 truth-hit windows have any nearby shadow
candidate at or above the shared 7 dB birth threshold. Candidate-family score
calibration matters: the same numeric threshold is not evidence of a matched
false-alarm operating point. Native versus shadow candidate counts are
9,604/19,392, 4,722/10,452 and 2,358/6,190. No threshold was tuned to this cohort.

The replay's shadow track/ACTIVE/COASTING counts exactly match every live
endpoint before any collection gap: 230/230, 122/122 and 67/67. This validates
the adapter against the recorded shadow tracker, without establishing GPU
parity for native inference.

## Resource and remaining experiment boundary

The heavy gate failed: available RAM 16.6 GiB versus the 48 GiB floor; largest
AMD GTT allocation 94.4 GiB; free disk 203.6 GiB passed. Qwen/Gufo stayed
loaded. No heavy IQ/CAF replay or service/configuration change occurred.
The small CPU adapter was compiled on CPU 8 at nice 19 with 768 MiB virtual
memory, 45 CPU-second and 120-second wall limits. Scoring/replay used the same
memory/core bounds and 60 CPU-second/120-second wall limits; the completed
analysis took under one second. A transient systemd limit attempt failed
before starting work; process-level limits supplied the actual bounds.

Retained exact-input inventory includes the HP 20-frame resident snapshot
sequence and its 240,246,288-byte IQ clip at
`/var/tmp/vectorwarp-hp-replay-strix-20261112`, plus the eight-frame,
98,283,452-byte CAF artifact `/var/tmp/hp-weak-evidence-live-20261113T2007.vww`.
These short development clips do not meet the depth-8 lifetime floor, supply
independent covariance fitting, or provide nuisance exposure. Normalized-gate
and four-arm guided comparisons were therefore not claimed as rerun RF
acceptance experiments. Cancellation arm C was not repeated.

Next evidence required: freeze untouched whole passes with full warmup/tails
(initially 30–50 per site), independent score-family calibration, and hours of
adjudicated nuisance/control exposure. Gate trials must hold the detections
and policy fixed while comparing the prior rectangle, both area-matched
controls, current fixed circle, and independently fitted covariance only if
a separate fit cohort exists. Guided arms additionally require exact retained
same-IQ joins and disjoint-credit eligibility throughout the policy horizon.

The next inexpensive development ablation is depth-4 candidate deduplication
before the same frozen tracker, with baseline retained, identical endpoints,
unchanged 7 dB/4-of-6 policy and explicit duplicate/unmatched reporting. Freeze
the suppression geometry before scoring a new snapshot. This addresses the
observed duplicate-track burden; it does not replace independent calibration
or the whole-pass safety study.

## Artifacts and reproduction

- Full report: `/var/tmp/shadow-rerun-v9-20261114.ltKwbp/results/report.json`;
  SHA-256 `d551abe8affd2b415038620d1f4e5f2b0f65a0e113595fac254fcc64b9e9e8fe`.
- Six truth-free replay inputs and six full outputs are beside that report.
- Runnable manifest: `docs/SHADOW_RERUN_EXECUTION_20261114.json`.
- Code: `script/rerun-temporal-endpoints.py`, `tools/TemporalEndpointReplay.cpp`.
- Four qualification/effect tests pass; the adapter also passes explicit
  fourth-measurement confirmation, reset and below-birth-threshold checks.

Exact bounded reproduction from the working directory:

```sh
ulimit -v 786432
ulimit -t 60
taskset -c 8 nice -n 19 timeout 120 python3 script/rerun-temporal-endpoints.py /var/tmp/shadow-rerun-v9-20261114.ltKwbp/source.jsonl --tracker /var/tmp/shadow-rerun-v9-20261114.ltKwbp/temporal-endpoint-replay --output-directory /var/tmp/shadow-rerun-v9-20261114.ltKwbp/reproduction
python3 -m unittest discover -s test/script -p test_rerun_temporal_endpoints.py -v
```

No release, commit, push, PR, model promotion, or live inference change.
