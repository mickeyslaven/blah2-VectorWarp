# Experimental guided confirmation — 2026-11-12

Status: source-only experiment. Guided promotion remains disabled. No live
activation, threshold relaxation, deployment or release is authorized by this
implementation or its focused tests.

## Two explicit policies

With `track_guided_shadow.promote=false`, the ordinary measured-hit M-of-N
confirmation and hard N-CPI tentative lifetime are unchanged. Detector seed
counts once; promotion occurs as soon as M measured hits fit within N frames.
Checkpoint schema 3 retains measured/source/support provenance. Shadow-only
guided evaluations never vote or change association/confirmation state.

With `promote=true`, checkpoint schema 4 counts only independent temporal
support credits. It does **not** scale N, extend tentative lifetime, lower a
detector threshold, or change PFA. Native, wire and API admission require
`N >= 2*M-1`: the shortest production support is two CPIs. Consequently HP's
ordinary **4-of-6 configuration cannot enable this experiment**. Tests use
feasible 2-of-3 and 4-of-7 configurations; these are tests, not recommended
live settings. Longer qualifying banks can make a feasible configuration
fail to confirm, conservatively.

## Support and credit

Postprocess flags retain all qualifying baseline/2/4/8-pass identities. The
tracker uses the maximum qualifying support depth, including the ordinary
two-CPI path. Missing/over-depth experimental detector support is rejected.
For an endpoint t and support depth d, credit represents `[t-d+1,t]`.
It is allowed only if none of the previous d-1 endpoints has already received
credit. This condition is sufficient for chronological contiguous windows:
any earlier window ending before the new window starts cannot overlap it.
The same check covers detector-after-guided, guided-after-detector, and
longer-bank overlaps. Predictions/coasts never receive a credit.

An overlapping real detector association still updates kinematics and keeps
its measured provenance; it does not add a confirmation vote. An overlapping
guided result remains shadow evidence, does not sustain a track and does not
reset its miss count. Neither source changes the final N-frame expiry rule.

Private history encodings:

| Field | Ordinary schema 3 | Experimental schema 4 |
| --- | --- | --- |
| Association state | bits 0–1 | bits 0–1 |
| Detector measurement | bit 2 | bit 2 |
| Guided prediction | bit 3 | bit 3 |
| Independent credit | not used | bit 4 |
| Support depth 1/2/4/8 | bits 4–7 | bits 5–8 |

Schema 4 header word 26 binds the temporal-ring counter. The validator checks
credit/support combinations and overlapping retained credits. Ordinary and
experimental checkpoint schemas are not interchangeable. Public schema-3/4
track JSON supplies `measured`, `source`, and `associated_measured`; guided
predictions cannot become measured scorecard observations or bearing samples.

## Gaps, restore, bounds

Experimental updates require adjacent logical generation, consumed-CPI
sequence and temporal-ring counter, with advancing timestamps. An exact
duplicate is a no-op only if all those identities, including the ring counter,
match. Discontinuity rejects the transaction; the caller must use the existing
input-reset path, which clears tracker, reference, power and angle history.
The acquisition layer remains responsible for detecting raw sample gaps and
requesting that reset; logical CPI counters are not a substitute for source
sample provenance.

A tracker-only checkpoint cannot restore temporal power/angle buffers. Thus
experimental restore permits only an identical currently committed warm
checkpoint with the same temporal counter. Cold, stale and cross-reset restores
fail closed and require an input reset. Ordinary restore behavior is unchanged.
The policy bit is immutable configuration, not the mutable guided-work budget
status; a timing trip cannot silently switch confirmation policy.

Storage does not grow: the existing bounded history and one reserved header
word carry lineage. Each decision scans at most seven prior entries; restore
validation adds at most seven checks per retained entry. The existing track
capacity, N<=255, tentative age, queue/device limits and guided budget remain.

The review also found a separate constructor overrun: disabling stationary
clutter allocated 16 bytes but still initialized its expanded 40-byte retirement
header. Allocation, device-limit checking and memory accounting now share
`persistenceBytes()`, which reserves the complete ten-word header even when the
filter is off. This fixes a real source defect; any remaining driver/harness
failure must be investigated independently rather than attributed to it.

## Required acceptance

The pure production integer functions are tested through
`test/script/test_gpu_tracker_lineage_contract.py`; sanitizers are enabled with
`LINEAGE_SANITIZE=1`. This is not GPU or RF qualification. The explicit
`testGpuTrackerCorrectness --lineage-only` CPU-Vulkan fixture exercises mixed
sources, impulse duplication, expiry, duplicates, reset/restore, gap rejection
and ring wrap. Serializer, native admission, resident wire and API tests cover
policy/provenance compatibility and the infeasible HP configuration.
`--confirmation-only` isolates ordinary 4-of-6 behavior with shadow off/on;
ordinary restores reject guided histories from the earlier schema-3 candidate.

Live promotion still requires complete behavioral GPU acceptance, exact-IQ
sample-disjoint controls, false-track exposure with multiple hypotheses and
repeated tests, target-driver/cadence qualification, and explicit permission.
Per-track disjoint credit is not a proof of global PFA or cross-track evidence
exclusivity. The earlier allocator-corrupting llvmpipe full-suite run remains
non-acceptance; it must not be described as a passed behavioral test.

## Bounded validation receipt — 2026-11-12

Display2 CPU-Vulkan (llvmpipe), container limited to two CPUs/4 GiB, now passes
the complete production-shader correctness fixture after the header fix:
six threshold variants, 28 exclusive-admission cases, eight ordinary
confirmation variants (including the measured RPA5731 four-hit burst),
151 non-square clutter/edge oracle cases, and the experimental mixed-window,
impulse, boundary, expiry, duplicate, reset/restore, gap and ring-wrap checks.
The isolated `--confirmation-only` and `--lineage-only` modes also pass.
Logs are on Display2 under `/var/tmp/vectorwarp-lineage-test/build/`:
`tracker-full-final.log`, `confirmation-final.log`, `lineage-second.log`.

The cold-cache lineage attempt (`lineage-first.log`) correctly tripped the
unchanged 10-ms guided-work safety cutoff during llvmpipe's first-dispatch JIT
compilation and failed to confirm. The second attempt used a bounded 64-MiB
Mesa shader cache and passed without disabling or raising that cutoff. This
establishes software behavior, not cold-start or target-GPU cadence acceptance.
No RF replay, production promotion or live restart was performed by this work.

Additional passes: 30 GLSL variants; 2,616 actual production integer-contract
cases under ASan/UBSan; serializer compatibility; tracker admission; resident
wire (202 checks/108 rejections); API configuration (743 typed leaves),
track-population and evidence-statistics tests; seven calibrated-replay
analysis tests; `git diff --check`. Public provenance negative tests ensure
guided-only ACTIVE rows do not become scorecard measured hits. Ordinary
schema-3 and schema-4 detector rows serialize identically after stripping
private credit/depth metadata.

The source-only policy is implemented and software-qualified within these
bounds. Live promotion is still **NO-GO** pending the RF/false-track and
physical-driver/load gates above and explicit authorization. The current HP
configuration has stationary clutter enabled; the disabled-buffer overrun
must not be used to explain that configuration's weak tracking without
separate evidence.
