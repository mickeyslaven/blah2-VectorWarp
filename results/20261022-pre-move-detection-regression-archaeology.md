# Pre-move detection regression archaeology (2026-10-18 roof move)

## Bottom line

The last release boundary before the physical move was `v0.1.20`
(`1bdfecc2ce6e69bc0677442a4029b2f3b9e7f935`, tagged 2026-10-17 20:28 EDT).
The strongest temporally aligned detection/tracking change in it is the new
GPU-resident persistent-clutter initiation filter:

- `c56248a31087c6593c6cfd2d8affa5283d990393` (2026-10-17 11:20) made it
  **enabled by default whenever `performance.acceleration` is `gpu-resident`,
  even when `stationary_clutter` is absent from YAML**;
- `e96cf3d2673792daa9974a40b0349d0ef8fa70d1` (11:32) changed learning from
  exact cells to an entire delay column inside the low-Doppler band and raised
  the default persistence to 60 CPIs;
- `c13ebd485d68173d82610989957ea86e9999f1e1` (11:38) made evidence accumulate
  across consecutive observations.

This feature cannot suppress CFAR plots or `/api/detection`: it runs in
`core/GpuTracker.private.h` after association and only prevents unmatched plots
from initiating new native tracks. Established tracks associate before the
filter. It can therefore explain “detections visible but no new track,”
especially near zero Doppler after 30 seconds of recurring occupancy, but not
“no detections.” The default-on behavior is a material silent compatibility
change, but the inspected code implements the documented policy; no new code
defect was established.

The other last-minute feature is unassisted/hybrid track publication,
`92bf429e486fe6300fd44844bfca056b31240c24` (2026-10-17 18:21), also included
in `v0.1.20`. `api/tracker-publication.js` leaves native processing and raw
detections untouched, but in `hybrid` mode it omits native tracks with fewer
than `native_min_associations` (5 in the retained 2026-10-21 config) from the
public `/api/tracker` frame while `/api/tracker/native` remains complete. This
can look like a detection loss in a track-driven UI. The code default is
`native`, so source presence alone does not prove it was active at move time.

The only 2026-10-18 commit, manifold automation
`3429d74d026368febcdc10c906432fd9489b250a`, is bearing/calibration lifecycle
work and is not contained in `v0.1.20`. It does not change the detector,
clutter canceller, or native tracker. It is not the suspected detection feature.

## What is proven about deployment and configuration

| Time (EDT) | Evidence | Detection relevance |
|---|---|---|
| Oct 15, 16:15 | `v0.1.17` / `58412855`; `docs/releases/0.1.17.md` | Introduced optional coherent beam fusion and multi-CPI controls; example profile remained `noncoherent`, one CPI, no beam gate. |
| Oct 16, 15:37–15:58 | `98dfaacd`, then `75bb8389` | Added TBD, then explicitly restored the conventional two-CPI baseline detector as an additional pass. TBD is opt-in. |
| Oct 17, 11:20–11:38 | `c56248a`, `e96cf3d`, `c13ebd4` | Persistent-clutter initiation became opt-out in GPU-resident mode. This is the closest automatic suppressor to the move. |
| Oct 17, 18:21 | `92bf429e` | Added opt-in hybrid publication; public-track filtering only. |
| Oct 17, 20:28 | `v0.1.20` / `1bdfecc2` | First tagged pre-move source containing both changes above. |
| Oct 18 | `results/cass-config-20261021.json` operator note | Array raised about one foot without intentional rotation. No retained package receipt, service journal, or exact Oct-18 config was found, so the installed tag/config at the instant of the move cannot be asserted from this checkout. |
| Oct 18, 15:19 | `3429d74d` | Manifold automation only; not in `v0.1.20`. |
| Oct 19 survey | `docs/FORD_STREET_BEARING_SURVEY_20261019.md` | Measured roof/site response changed: KDKA channel phases shifted roughly 40–100 degrees on several channels and amplitude response changed. |
| Oct 21 snapshot | `results/cass-config-20261021.json` | Retained post-move config is GPU-resident, array-eigenbeam reference, 12-beam coherent fusion, effective beam trials 5, PFA 0.005, min Doppler 0, TBD enabled, adaptive tracker, and hybrid publication. `stationary_clutter` is absent, which activates its code default. This proves later activation, not Oct-18 activation. |

Git ancestry proves the three persistent-clutter commits and `92bf429e` are in
`v0.1.20`; manifold automation is not. Git history is not an installation
receipt, so “deployed immediately before the move” remains a high-confidence
release inference rather than a verified host fact.

## Ranked regression candidates

1. **Changed physical response interacting with coherent fusion/reference
   synthesis — highest for missing raw plots.** The retained survey directly
   measures a changed installed response after the move. Coherent fusion
   (`223068f` through `5e658db`) changes the statistic before CFAR and divides
   the configured PFA by `effective_beam_trials`; with the retained config the
   conventional coherent pass uses 0.005 / 5 = 0.001. Array-eigenbeam reference
   synthesis can also absorb common spatial energy. This stack predates the
   move and was opt-in, so the missing Oct-18 config is the key provenance gap.
   Later controlled replay is mixed but argues against a simple software
   rollback cure: the r277 positive is retained, while noncoherent and
   noise-whitened fusion did not reveal the studied misses
   (`docs/research/20261021-ford-street-recovery.md`).

2. **Persistent-clutter initiation default — highest for plots-without-new-
   tracks and the best match to “major feature just before the move.”** It is
   automatically active in GPU-resident mode. The Oct-17 implementation learned
   a whole delay column for recurring low-Doppler detections, a broader scope
   than the exact delay-Doppler-cell implementation introduced later by
   `fdf5fd69b4824d1b898417f0293a6824f67707ac` on Oct 21. It does not alter raw
   detection output.

3. **Hybrid publication — high for public/UI track disappearance, zero for raw
   detection loss.** A fresh sidecar input causes only stable external tracks
   plus native tracks with at least five associations to be published. Native
   tentative/short tracks remain available at `/api/tracker/native`. The Oct-21
   snapshot has this arm active; activation at move time is unproven.

4. **Coherent CFAR/TBD trial accounting — medium-low.** Coherent fusion makes
   the baseline threshold more conservative by its effective beam trials. TBD
   adds separately trial-corrected 2/4/8-CPI passes, but commit `75bb8389`
   preserves the baseline pass, so enabling TBD should not remove a qualifying
   baseline plot after history is warm. Startup intentionally suppresses
   incomplete multi-CPI output (`02c3c63`, fixed coverage in `080f9aa`).

5. **Adaptive tracker gate / beam association — low on retained evidence.** The
   Oct-21 config activates `adaptive_gate`, but the current focused unit test
   proves it contains the legacy acceleration envelope and retains a weak target
   across missed CPIs. `beam_tolerance` is absent, so coherent-beam association
   gating is disabled. These affect tracking, not CFAR plots.

6. **ECA/clutter cancellation, gain normalization, delay/Doppler eligibility —
   no immediate pre-move regression found.** No ECA/Wiener-Hopf algorithm change
   landed immediately before the move. `8c48e244` (Oct 20) later added explicit
   clutter-range alignment; the retained config has matching ambiguity/clutter
   bounds. Per-channel physical gain support and reporting (`899d3b06`,
   `15bbc44f`, `db6dfc5c`) landed Oct 20, after the move. The retained config's
   `minDelay: 5`, `minDoppler: 0`, and ±600 Hz ambiguity range broaden rather
   than exclude the studied cells. The raw-IQ audit also found stable calibration,
   zero sample-lag span, negligible clipping, and strong array coherence in
   positive and miss captures.

## Safe replay/shadow ablation matrix (no live mutation)

Freeze one pre-move positive, one post-move positive, and at least two post-move
miss MCHQ clips with exact signal-time trajectories. Use identical CPI/range/
Doppler geometry and report raw truth-near plots, native tracks, and public
tracks separately.

1. **Production replay control:** exact retained post-move config and current
   binary; establish deterministic hashes/counts.
2. **Tracker-only clutter arm:** same detector maps, compare
   `stationary_clutter.enable: true` versus `false`. Acceptance signal is newly
   initiated truth-near native tracks with identical raw plot frames and no
   disproportionate control-track growth. This is the first rollback arm.
3. **Publication-only arm:** feed the same native frames to `mode: hybrid` and
   `mode: native`; compare `/api/tracker`, `/api/tracker/native`, and
   `/api/detection`. This requires no RF replay. It should localize UI loss
   exactly. This is the second rollback arm.
4. **Fusion arm:** coherent beam bank versus noncoherent on identical complex
   maps, using empirically matched whole-map false-alarm budgets (do not compare
   raw configured PFA values without accounting for five beam trials). Record
   the target-cell gain/loss and global control p99.
5. **Reference arm:** array eigenbeam versus a predeclared physical reference
   channel, with the same fusion/CFAR arm. Reject any arm that loses r277 or
   increases whole-map false alarms; prior target-protected reference searches
   already failed this positive gate.
6. **TBD arm:** enabled versus disabled after warm-up. The conventional pass
   should be bitwise/numerically invariant; investigate only if it is not.

Do not combine arms initially. If arm 2 changes tracks without plots, the cause
is initiation. If arm 3 alone changes the UI, the cause is publication. Only
arms 4–5 can substantiate suppression before CFAR. A safe operational rollback
recommendation should require the same arm to retain both positive controls and
improve opportunity-normalized held-out pass yield; no live change is justified
by archaeology alone.

## Bounded validation performed

- Directly compiled and ran `test/gpu/TestGpuTrackerAdmission.cpp`: pass,
  including legacy-envelope preservation, delayed weak-target retention,
  persistent-cell locality, intermittent recurrence, and stale expiry.
- `node api/config-manager.test.js`: pass.
- `node api/resident-config.test.js .`: 457 setting leaves plus boundary and
  cross-field checks pass.
- `node api/tracker-publication.test.js`: pass.
- `node api/tracker-forward.test.js`: pass.

No live RF, service, receiver, or configuration was read or changed. No clear
code defect met the bar for a patch; this report is the only intended artifact.
