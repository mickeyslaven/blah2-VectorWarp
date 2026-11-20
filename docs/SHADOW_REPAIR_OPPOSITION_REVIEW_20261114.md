# Oppositional repair/acceptance review — 2026-11-14

Scope: independent source and mathematical review of reset semantics,
track-aware birth suppression and a matched association-gate experiment.
No live inference, services, Qwen/Gufo, production code or git state changed.
This is not final acceptance of the six-part implementation, which was still
being edited during review.

## Actionable implementation findings

1. **Expose every semantic reset, including statistic-family changes.** In
   `LiveTemporalShadow.h`, `family_ != output->bootstrap.statisticFamily`
   resets both `temporal_` and `scorer_` without advancing `generation_`.
   Configuration/sample-lineage failures detected by `continuous()` can also
   reset without an explicit invalidation call. A new depth endpoint can
   realign to the former cadence, concealing the reset from generation-plus-gap
   replay logic. Add an output-bound reset serial/reason or advance generation
   safely while binding the accepted frame and output to that new generation.
   Do not increment generation after capturing it and then accidentally discard
   every valid reset output as stale. The existing family-change test verifies
   warmup but not externally observable reset identity.

2. **Correct the non-adaptive track work bound.**
   `TemporalShadowTracker::process` checks `priorTracks + candidates.size()`
   against `maximumTracks`. The accepted `adaptiveGate=false` configuration
   creates `2*floor(maxAcc*cpi*cpi)+1` acceleration hypotheses per birth.
   At maxAcc=10 and CPI=0.5, 256 candidates can create 1,280 tracks after passing
   a 512-track check. Multiply by the supported hypothesis count with checked
   arithmetic, or explicitly reject non-adaptive temporal-shadow tracking.
   The current v9 adaptive mode and its recorded rerun results are unaffected.

3. **If preserving tracks over observer-only gaps, age by missing scheduled
   updates or wall time.** Current CPU Tracker increments inactivity once per
   call, even when the supplied elapsed interval is arbitrarily large. A
   truth-free executable check confirmed a measured ACTIVE track remains
   COASTING after a single empty update 60 seconds later under depth4
   M4/N6/delete3. This is a conditional repair hazard, not a claim that the
   present reset-all implementation bridges such gaps. Preserved tentative
   tracks must also expire their M/N window. Gaps must never append measured
   history, create a support credit or combine noncontiguous CAF samples.

4. **Document and test the new aggregate work-budget semantics.** The current
   patch removes the successful-offer wall-clock dead zone, correctly avoiding
   loss of contiguous CPIs delivered in bursts while the worker is idle.
   `minimumIntervalMs` consequently becomes overload backoff rather than a
   successful-work duty bound. The existing `minimumIntervalMs >= 2*maximumWorkMs`
   validation no longer enforces that duty fraction. Retain one-worker/copy
   limits and verify aggregate work exposure on shorter-CPIs/backlogged input,
   or add a rolling work budget that reports its deliberate gaps. Do not retain
   a duty-bound claim supported only by the obsolete interval inequality.

## Recommended birth-only contract

The prior frozen full-list suppression reduced duplicate/unmatched output but
lost measured ACTIVE truth support. The next implementation must preserve all
raw candidates for existing-track association.

1. Associate existing tracks first against the complete raw candidate array,
   retaining baseline gates, ordering, ranking and exact-candidate one-to-one
   claims. Existing `doNotInitiate` already prevents one exact candidate from
   serving multiple tracks; it does not prevent adjacent-cell births.
2. After all associations finish, create a separate birth-reservation mask
   around actual claimed measurements. Never reserve a neighborhood merely
   because an unmeasured prediction or COASTING track lies nearby: that can
   prevent a real target from being born while a stale hypothesis persists.
3. On the remaining birth-eligible candidates, apply a predeclared bounded
   suppression rule and deterministic SNR/delay/Doppler/original-index ties.
   Keep original coordinates, SNR and support lineage. One spatial birth may
   own one independent credit; neighbors must not multiply confirmation.
4. Keep assignment exclusivity and birth reservations separate. Marking every
   neighbor claimed before other existing tracks associate would reintroduce
   the measured-support loss of full-list filtering. Two existing nearby
   targets must still be able to claim two distinct candidates.

The initial ablation should change birth admission only. A global assignment
solver, ACTIVE-first priority, covariance estimator or reordered existing-track
iteration is a separate factor. Connected-component clustering can transitively
merge distant peaks through an intermediate ridge; use an explicitly bounded
representative radius if clustering is chosen, and test chains. Report every
suppressed birth reason and candidate-owner relationship in replay diagnostics.

## Gate matrix: geometric area is not empirical burden

Use normalized residuals `x = delay_bin_error`,
`y = Doppler_error_hz * base_CPI_seconds`. Hold candidate arrays, endpoint
cadence, adaptive first-association branch, M/N/delete/birth policy and ranking
fixed. The current fixed circle uses identity covariance and is not a fitted
99% coverage region.

| Arm | Acceptance geometry | Area in normalized units |
|---|---|---:|
| R4 | `abs(x)<1 && abs(y)<1` | 4 |
| C4 | `x*x+y*y <= 4/pi` | 4 |
| C29 | `x*x+y*y <= 9.210340371976184` | 28.93513764966186 |
| R29 | `abs(x)<2.6895695589472055 && abs(y)<2.6895695589472055` | 28.93513764966186 |

C29 has 7.2337844124 times the old rectangle's area. Its current area ceiling
of 100 does not constrain it. Preserve implementation boundary semantics and
the tiny numerical covariance loading in exact parity tests.

Even equal-area arms are not equal-burden arms on a structured, discrete CAF.
At an exactly grid-centered prediction, the four geometries admit 1, 5, 29
and 25 integer lattice points respectively. This was checked directly; it is
an illustration, not an observed nuisance rate. Expected clutter matches are
the integral/sum of the actual nuisance density over the accepted region,
not area alone. Report in-gate pairs, actual claims, unmatched births,
unmatched ACTIVE exposure and adjudicated false tracks alongside continuity.

An equal *empirical* burden matrix requires an independent nuisance calibration
cohort to choose each gate's threshold against the same frozen false-association
budget. Freeze thresholds before held-out target/control scoring. The existing
shifted-truth checks are not such a calibration cohort. Covariance gating is
eligible only with independent whole-pass residual fitting; fix coordinate
units, bias handling, covariance conditioning, prediction-age behavior and
area ceiling before holdout. Do not estimate covariance or tune thresholds
from the scored truth labels.

Run the four geometry arms under unchanged birth admission first. If the
birth-only repair passes its own tests, evaluate a second 4-arm block with
that exact frozen birth policy. This separates gate geometry, gate volume
and birth suppression instead of combining them in one unexplained winner.

## Required acceptance tests

- Reset visibility: changing one statistic-family cell, capture epoch,
  configuration epoch, sample geometry, duplicate sequence or real sample gap
  produces an explicit reset identity; contiguous burst-delivered CPIs do not.
- In-flight invalidation: an older completion is never published after a new
  generation/reset; cumulative counters persist while current gauges clear.
- Missing updates: one skipped depth endpoint does not fabricate a measurement;
  gaps longer than the deletion/N horizons expire appropriate state. Exact
  disjoint support lineage remains mandatory after rewarm.
- Birth-only invariance: with no births admitted, existing-track associations
  and measured history are byte-equivalent to baseline. A stronger neighboring
  point outside a track's gate cannot remove its weaker valid association.
- Reservation: an actually claimed patch prevents redundant births; an
  unassociated/coasting prediction does not. Two distinct existing targets can
  still claim distinct nearby measurements. Test deterministic equal-SNR ties,
  chain bridging, boundary distances and candidate permutation.
- Exclusive evidence: no candidate is claimed twice, no new birth duplicates
  a claimed measurement, and no neighbor duplicates an independent M/N credit.
- Capacity: non-adaptive hypothesis multiplication, zero capacity headroom and
  arithmetic overflow fail before exceeding declared work/memory bounds.
- Gate parity: axes, corners, boundary points, receiver-dependent bin scales,
  base-CPI versus update cadence, invalid covariance and area cap. Include the
  adaptive first-association path so a mature-gate change cannot silently
  change acceleration estimation. Confirm CPU/resident behavior separately.
- Evaluation: retain raw input and all rejected/overflowed episodes, use
  untouched whole passes and independent nuisance exposure, report measured
  continuity, ID fragmentation/switches, confirmation delay, misses, duplicate
  burden and false tracks. Longer coast lifetime is not measured improvement.

## Review receipt

Source hashes observed during review (concurrent edits may supersede them):

- `LiveTemporalShadow.h`: `be0e8b63e0f35c6e36577ea97d41073a015ce7d11003a58200163c8d57113c4f`.
- `TemporalShadowTracker.h`: `7fffc58868a87cc495e552ddf4af0948a7b14b065eec3d27193b2b13de530a0a`.
- `Tracker.cpp`: `e39d350e52b398020c70d85bd1adba18954912e45f6e2d895e7f633e99d16001`.
- `NormalizedInnovationGate.h`: `8d6d4b53d49d8516f2213a534af599d695b3ac446a2f5b0d2e2a83001a8c824b`.

The long-gap check used the previously compiled production CPU adapter at
`/var/tmp/shadow-rerun-v9-20261114.ltKwbp/temporal-endpoint-replay`
(SHA-256 `8c1440044dc826dc8c974ce06b04959a4cff92233a9db04666247eda795e484e`):
four identical 8-dB measurements at 1/3/5/7 seconds, followed by an empty frame
at 67 seconds; policy M4/N6/delete3, base CPI .5. The output was ACTIVE then
COASTING. No heavy tests/builds were run. All findings were sent to the parent
implementer promptly; re-review the final patch against these contracts before
claiming implementation acceptance.

### Concurrent repair review addendum

The parent began adding `resetSerial`/`lastResetReason` and a non-adaptive
birth multiplier while this review was underway. Two immediate corrections
were sent:

- The work-budget exception branch needs braces around both
  `budgetExhausted=true` and `resetReason="work_budget_exhausted"`; otherwise
  the following `else` has no matching `if`.
- The multiplier must reproduce Tracker's floating-point operation order,
  `floor(maxAcc / (1 / (cpi*cpi)))`. Algebraically rewriting it to
  `floor(maxAcc*cpi*cpi)` is unsafe at a bin boundary: CPI .3 with
  maxAcc 11.11111111111111, and CPI .6 with maxAcc 2.7777777777777777, yield
  one production acceleration bin but zero with the multiplication order,
  undercounting three hypotheses as one. Add that exact boundary regression.

The new reset identity must reach API status, collector records and replay
segmentation. A status-only serial does not fix offline replay identity.
These are intermediate-patch findings, not assertions that the final repair
still contains them; verify the final diff/tests before acceptance.
