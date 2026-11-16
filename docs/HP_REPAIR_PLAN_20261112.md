# HP evidence repair plan — frozen before further scoring, 2026-11-12

No production deployment/restart, threshold/birth-SNR/PFA change, speculative
gate widening, guided promotion, commit or release. Source work is in the
candidate tree; bounded tests/replays run on Display2 only.

1. Version the causal-funnel wire contract with exact unsigned decimal
   nanoseconds. Keep safe legacy v1 validation; reject rounded numbers,
   malformed/overflow times and truth-shaped input. Native time precision and
   capture/generation identity must remain explicit.
2. Connect opt-in, bounded native candidate/admission/association/transition
   diagnostics. Report retained-prefix coverage and drops; diagnostics cannot
   influence radar decisions or stop radar on recorder failure. Keep a separate
   evaluation-only ledger for truth/geometry/association exclusion counts and
   bounded examples. Never send that ledger into inference.
3. Freeze offline arms: unchanged ordinary two-CPI baseline; existing
   fractional 2/4/8-CPI TBD; diagonal post-clutter statistic; fixed0.75-shrinkage
   post-clutter statistic. Preserve the baseline in every comparison and keep
   covariance/statistic-family calibration separate from raw-power Gamma CFAR.
   No parameter choice may use held-out outcome data.
4. Require identical byte-verified IQ across arms and sample-/whole-pass-disjoint
   calibration, holdout and qualified negative controls. MXY1663, AAL743 and
   SWA4870 r120 are already-inspected development diagnostics, not untouched
   held-out tests. Preserve this distinction. Count only disjoint temporal
   support; report detector, native tracker and evaluation exclusions separately.
5. Run bounded deterministic instrumentation/runner tests, then only the cohort
   arms whose geometry, warmup, split and negative-control gates pass. Missing
   controls or insufficient clip duration are explicit blockers, not zero false
   tracks. Report false-track delta as unavailable unless independently measured.
6. Implement an additional native inference change only if paired retained
   evidence and negative controls prove the defect and support its correction.
   Existing corrected M-of-N remains; noise-sensitive gates stay unchanged.

Acceptance: exact-time round trips and adversarial schema rejection; byte/
queue/disk bounds; ordinary output/decision parity with diagnostics off/on;
native generation/reset/restore/overflow tests; evaluation eligibility counts
unchanged; leakage/overlap/short-warmup manifest rejection; immutable arm
receipts and artifacts; honest no-go if RF/negative exposure remains inadequate.
