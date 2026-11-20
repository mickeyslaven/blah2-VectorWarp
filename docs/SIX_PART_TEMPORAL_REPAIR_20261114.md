# Six-part temporal repair — 2026-11-14

No production detector/tracker policy is promoted by this work. ADS-B remains
evaluation truth only and never enters radar inference.

## Implemented

1. Successful observer work no longer starts a pacing dead zone. The configured
   minimum interval is now copy-overload backoff only. The live fault signature
   before this change was 124 `minimum_interval` drops/resets with zero worker,
   budget, copy, or contention drops.
2. Every semantic temporal reset now exposes a monotonic `reset_serial` and
   `last_reset_reason`. The collector persists the serial and both replay tools
   split episodes on it, including statistic-family resets that retain the same
   capture generation.
3. Depth 4 is the first tracker-policy comparison lane. Depth 2/8 remain controls.
4. An opt-in birth-only exclusivity policy associates existing tracks against
   the full raw candidate set, then reserves neighborhoods around actual claimed
   measurements and stronger selected births. Coast-only predictions cannot
   suppress births. The default remains off.
5. The tracker exposes a configurable normalized NIS threshold. The replay
   matrix compares the legacy strict square, equal-geometric-area circle
   (`NIS=4/pi`), current circle (`NIS=9.210340371976184`), and current circle
   plus birth exclusivity. Equal area is explicitly not called equal empirical
   nuisance burden.
6. New tools freeze per-depth score calibration on whole-pass-disjoint nuisance
   splits and certify uninterrupted declared whole passes, complete export,
   invariant runtime identity, the 80-base-CPI structural floor, evaluation-pass
   count, and adjudicated nuisance hours.

The temporal shadow's non-adaptive work bound now uses the Tracker's exact
initiation-hypothesis operation order. This closes a separate potential
`maximumTracks` undercount while preserving the live adaptive configuration.

## Frozen diagnostic matrix

Source: `/var/tmp/shadow-rerun-v9-20261114.ltKwbp/source.jsonl`.
Receipt: `/var/tmp/depth4-tracker-matrix-20261114.pW4cQg/report.json`.
This is a reused development cohort, not acceptance evidence.

| Depth-4 arm | Truth ACTIVE endpoints | Duplicate truth ACTIVE endpoints | Unmatched ACTIVE endpoints | Longest measured continuity |
|---|---:|---:|---:|---:|
| Legacy rectangle | 10 | 3 | 102 | 10.0 s |
| Equal-area circle | 21 | 6 | 111 | 18.0 s |
| Current circle | 43 | 76 | 177 | 30.0 s |
| Current circle + birth exclusivity | 42 | 20 | 118 | 30.0 s |

Birth-only exclusivity preserved 42/43 truth ACTIVE endpoints and the full
30-second longest continuity while cutting duplicate endpoints 76→20 and
unmatched endpoints 177→118 on this cohort. This is promising enough for a new
shadow cohort, not live promotion. The equal-area circle produced 1,863 in-gate
pairs versus 1,849 for the legacy square here; the current circle produced
3,639. Empirical burden still requires independent nuisance exposure.

## Validation completed

- Focused C++ adapter compilation succeeded.
- Live observer header syntax compilation succeeded.
- Birth-exclusivity and equal-area smoke replays succeeded.
- Calibration and whole-pass validator smoke checks succeeded.
- Oppositional probes initially exposed acceptance-validator aliasing, repeated
  pass counting, and empty-provenance acceptance. The validator now requires
  nonempty pinned runtime identity, counts unique pass IDs, and rejects aliased
  overlapping source intervals across pass identities; all three adversarial
  probes pass after repair.
- Python compile and `git diff --check` succeeded.
- Full unit execution and a clean live cohort remain required before acceptance.

## Live observation-only deployment

HomePortal v10 binary SHA-256:
`db9f71036ef8051c36df639ca46b5487de5f2c9cc4fac302ee9e847ad0ae8bbf`.
It was built on HomePortal after an HP-produced candidate was rejected before
restart for a newer-glibc ABI dependency. Receiver and acceleration capability
reports matched v9 before restart. At 168 accepted CPIs after warmup, v10 had
zero drops of every reason, zero rejections, zero budget exhaustion, one initial
`input_discontinuity` reset, eight CPIs available, and GPU execution true on all
three illuminators. The new bounded ledger is under
`/var/lib/vectorwarp-api/temporal-pairs/` (private basename omitted).
The collector was initially capped at 1 GiB, then raised to 32 GiB after the
observed ledger rate showed that 1 GiB would not cover the required multi-day
exposure. At roughly 2.7 GiB/day, 32 GiB provides about 11 days while the
filesystem had 271 GiB free. HP remained live on its existing guided
build; no HP detector policy was changed by this observer repair.

The portable collector required an explicit `--illuminator` runtime lane ID.
The recorded deployment used two explicit environment values equivalent to:

```ini
VECTORWARP_TEMPORAL_ILLUMINATOR=TX-C-RF23
VECTORWARP_TEMPORAL_OUTPUT=/var/lib/vectorwarp-api/temporal-pairs/node-a-tx-c-v10.jsonl
```

These are illustrative values retained to explain the evidence provenance, not
an installation template. The supported experimental controls now live in the
normal configuration and UI rather than a dated service override.

Oppositional review: `docs/SHADOW_REPAIR_OPPOSITION_REVIEW_20261114.md`.
