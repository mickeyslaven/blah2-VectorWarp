# Shadow experiment audit — 2026-11-14

## Decision

No recovery shadow reviewed from the prior several days has earned production
promotion. Keep the observer lanes running, but keep their detector/tracker
feedback disabled until the acceptance experiment below passes.

## Corrected evidence

The newest HomePortal WPXI pair ledger contains 2,349 records. The old analyzer
used a 5–545 km default denominator even though the configured searchable lane
was approximately 0.781–59.968 km. Re-analysis over the actual search extent:

| Depth | Opportunities | Native hits | Shadow hits | Shadow-only | Shifted control |
|---|---:|---:|---:|---:|---:|
| 2 CPI | 10,894 | 665 | 671 | 191 | 42 |
| 4 CPI | 5,089 | 318 | 384 | 167 | 21 |
| 8 CPI | 2,432 | 148 | 181 | 88 | 11 |

The 4/8-CPI lanes are weak positive development evidence, not a promotion
result. The ledger predates the current runtime, has collection gaps and reset
generations, exports only the top 16 shadow candidates, and does not provide an
independent false-track exposure.

## Defects found

1. HomePortal shadow tracking forced a -100 dB birth threshold while production
   used 7 dB. This was not a like-for-like comparison and could exhaust the
   bounded shadow tracker.
2. The worker latched off after one rejection without retaining the exception
   reason in status. The API therefore showed a failure with an empty reason.
3. The pair analyzer's fixed physical denominator overstated the trial space.
4. The old collector lacked immutable processor/config provenance and could
   consume a frame key before the record was durably written.
5. Shift-by-17 truth is a sensitivity check, not an independent nuisance or
   false-track control.
6. Earlier HomePortal and HP summaries included already-invalidated geometry or
   row-index joins. They are not promotion evidence.
7. Narrow cancellation arm C increased detections mostly on a stationary comb;
   it should be rejected. Loaded cancellation, coherent integration, HP phase
   correction, and diagonal/shrinkage statistics remain research-only.

## Repairs in this worktree

- Live shadow tracking now inherits the production birth threshold by default.
  An explicit `minimum_initiation_snr_db` experiment override is available in
  the generic configuration/UI for every supported receiver profile.
- Worker failures retain their exact bounded failure category in status.
- The analyzer requires an explicit or recorded searchable delay extent and
  retains the lane output rate for candidate-bin conversion.
- The collector records processor/config identities, requires matching live
  config revisions, records the searchable extent, and marks a key consumed
  only after a flushed write.
- Guided tentative promotion/sustain and temporal association sustain remain
  disabled on the live nodes.
- HomePortal v9 exports every bounded candidate up to 256, records actual
  candidate counts and tracker policy horizons, uses the base CAF CPI for
  Doppler resolution, and writes a clean provenance-bound ledger.

## Required acceptance experiment

Freeze identical inputs and policies, then compare four arms: ordinary
baseline, changed M/N only, disjoint-credit only, and guided sustain/promotion.
Use held-out whole passes and real nuisance exposure. Report measured ACTIVE
continuity, fragmentation, confirmation delay, missed eligible intervals, and
adjudicated false tracks per hour. A detector/tracker shadow may be promoted
only when it improves those locked outcomes without increasing false tracks or
violating the live processing budget.
