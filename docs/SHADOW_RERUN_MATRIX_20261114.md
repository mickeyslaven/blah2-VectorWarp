# Shadow experiment rerun matrix — 2026-11-14

Oppositional review found that several negative or positive conclusions tested
measurement defects along with the intended algorithm. This matrix is the
minimum rerun order. It does not authorize promotion or release.

| Priority | Experiment | Existing result | Required action |
|---|---|---|---|
| P0 | HomePortal 2/4/8-CPI candidates, v2–v7 | Invalid for complete recall or promotion: wrong legacy range denominator, top-16 truncation, gaps and incomplete provenance | Use clean v9 ledger with full bounded candidates and exact search extent; independently calibrate each statistic family and add real nuisance controls |
| P0 | 2/4/8-CPI shadow tracking | Invalid comparison: birth threshold differed and tracker used CPI×depth as Doppler resolution | v9 fixes threshold and gate resolution; replay each depth against an ordinary tracker sampled at identical endpoints and policy, reporting wall-time horizons separately |
| P0 | Normalized association gate | Inconclusive wider gate, not calibrated uncertainty | Compare prior gate, equal-area controls and independently fitted covariance on identical IQ; score correct associations, continuity, identity switches and false tracks |
| P0 | Guided sustain/promotion | Mechanism tests valid; RF benefit unproven | Four frozen arms: baseline, M/N only, disjoint credit only, sustain/promotion; require measured continuity gain and no false-track increase |
| P0 | M/N and disjoint lineage | Software lineage tests valid; RF policy inconclusive | Preserve ordinary policy control; only test feasible disjoint-credit lifetimes and retain reset/overlap controls |
| P1 | Diagonal/shrinkage bootstrap and fractional TBD | Numerical gain diagnostic only; prior calibration/control insufficient | Exact-IQ replay with complete surfaces, frozen calibration and independent whole-pass holdout/nuisance cohorts |
| P1 | HP morning/subthreshold results | Development diagnostics only; old row-index/frame assumptions contaminate track claims | Rescore exact frame lineage, then use untouched whole passes for operational comparison |
| P1 | Cancellation arm C | Valid negative for retained cohort | Keep as nuisance stress control; do not rerun merely because counts rose |
| P1 | Cancellation loading D/E | Numerical stability valid; RF benefit unknown | Paired injected/uninjected and fixed/refitted replay followed by whole-pass detector/tracker comparison |
| P1 | Coherent integration | Inconclusive; prior anchors/nulls insufficient | Freeze a measured direct-path anchor, phase convention and search trials before held-out same-IQ replay |
| P1 | HP phase correction | Inconclusive and not a hardware-only correction | Fit known-geometry residuals or consistently transform downstream steering; freeze on independent observations and replay held-out IQ |
| P2 | Frequency-resolved observer | Valid diagnostic, not recovery evidence | Keep collection; qualify a frozen candidate only after its readiness gate passes |
| P2 | Doppler ridge scrubber | Inconclusive counts-only shadow | Full tracker replay including slow/crossing targets and nuisance exposure |

## Exposure floor

Depth-8 tracking with unchanged 4-of-6 needs at least 32 base CPIs before a
confirmation is possible. A complete warmup, confirmation horizon and deletion
tail needs about 80 base CPIs (roughly 40 seconds at a 0.5-second base CPI) per
episode; whole-pass coverage must extend beyond that. Start the paired effect
study with 30–50 untouched passes per site, then size the final cohort from the
observed effect. False-track acceptance needs hours of adjudicated nuisance
exposure; zero events gives an approximate one-sided 95% upper rate of
`3 / exposure_hours`.

## Valid work that does not need blanket repetition

- Warm software-Vulkan lineage and overlap tests.
- Cancellation arm C rejection on its retained cohort.
- Cancellation D/E numerical solve checks.
- Causal-funnel and suspicious-track retention instrumentation tests.

These engineering results remain valid unless their corresponding code changes.
