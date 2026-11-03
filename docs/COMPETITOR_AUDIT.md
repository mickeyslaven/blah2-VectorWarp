# Adjacent SDR passive-radar audit

Audited 2026-11-03 at [SDRmm `de175b4`](https://github.com/Newspicel/sdrmm/tree/de175b45c14ce7cb503c1a0b167aed4638e7979c) and [OnAir `b7cf9ac`](https://github.com/adswill/OnAir/tree/b7cf9ac054e6e392e30e6c265b5810071644f6f1).

## Scope

SDRmm and OnAir are broad SDR suites with passive-radar modules. They are not
direct VectorWarp substitutes, and repository popularity does not establish use
of either passive-radar module. This audit looks only for ideas worth testing in
VectorWarp; it does not copy code or authorize a production change.

| Project | Category | Evidence level relevant to this audit |
| --- | --- | --- |
| VectorWarp | Dedicated operational passive radar | Live processing, recording, replay, tracking, bearing and multi-SDR support |
| SDRmm | Broad SDR suite with a passive-radar module | Implemented processing and tests; this audit found no published field-performance study |
| OnAir | Broad SDR suite with an experimental passive-radar module | Its documentation says the module has only been tested with its simulator and never with a real radio |
| 3lips | Complementary multi-node position solver | VectorWarp can feed it; it is not a replacement for local radar processing |

Direct passive-radar products and research-only simulators were not compared in
this pass.

## Ideas that make sense

| Idea | Fit for VectorWarp | Decision |
| --- | --- | --- |
| OS/GO CFAR alongside the current delay-axis CA-CFAR | Potentially useful near non-uniform clutter or interfering targets. SDRmm implements CA, OS and GO choices. | **Evaluate offline.** Compare calibrated false-alarm rate, detection retention, edge behavior and processing time on the same recordings. Do not change the default from source inspection alone. |
| Covariance-aware tracking with global assignment | Potentially useful when tracks cross or several detections compete for one track. SDRmm uses a Kalman state and a global cost assignment; VectorWarp currently associates each track greedily within a fixed gate. | **Evaluate offline.** Require fewer identity swaps or longer truth-linked continuity without increasing false operational tracks or missing the CPI budget. |
| Additional cancellation methods | SDRmm exposes ECA-B, ECA-S, NLMS and block NLMS. OnAir implements ECA and NLMS, but its results are simulator-only. Different methods can trade cancellation depth, target preservation and compute. | **Benchmark, not a settings dump.** Add a method only if replay shows a useful operating region that the existing pipeline does not cover. |
| DAB clean-reference reconstruction | SDRmm reconstructs a DAB reference at 2.048 MS/s. This can help when DAB is the illuminator, but does nothing for VectorWarp's normal FM workflow. | **Defer.** Revisit only with a DAB deployment, suitable recordings and an end-to-end clean-reference test. |
| Quantitative synthetic and replay acceptance | Both projects test cancellation, detection and tracking with generated signals. VectorWarp already has OPRS, IQ injection and hardware-independent replay. | **Extend the existing VectorWarp path.** Do not create a second benchmark framework. Add scenarios only when they exercise an identified failure mode. |

## Ideas that do not make sense now

- Matching either suite's decoder catalog, radio workbench or plug-in UI would
  dilute VectorWarp's passive-radar focus.
- Treating OnAir's simulated cancellation, range or false-alarm numbers as a
  live benchmark would be invalid; its own documentation excludes real radios,
  antennas, clocks and moving clutter.
- Adding every exposed algorithm as a user setting before it beats a current
  profile would increase complexity without evidence of better radar results.
- Copying implementation code is unnecessary and would introduce AGPL/GPL
  licensing obligations. Any later implementation should be independent and
  based on published methods and VectorWarp tests.

## Required evidence before a feature PR

Use the same held-out recording set and fixed configuration for the baseline and
candidate. Include target-free clutter, weak truth-linked aircraft, persistent
near-source clutter and crossing/competing detections where applicable. Report:

- detections and false alarms with denominators;
- truth-linked confirmed tracks, false operational tracks and identity swaps;
- track continuity, initiation delay and coast behavior;
- cancellation depth and weak-target preservation for cancellation changes;
- CPI time, backlog and memory; and
- an unchanged-profile control run.

A candidate advances only when it improves its intended case without regressing
the control cases or exceeding the processing budget.

## Primary sources

- SDRmm: [project scope](https://github.com/Newspicel/sdrmm/blob/de175b45c14ce7cb503c1a0b167aed4638e7979c/README.md), [passive-radar guide](https://github.com/Newspicel/sdrmm/blob/de175b45c14ce7cb503c1a0b167aed4638e7979c/docs/src/user-guide/passive-radar.md), [CFAR](https://github.com/Newspicel/sdrmm/blob/de175b45c14ce7cb503c1a0b167aed4638e7979c/crates/dsp/src/radar/cfar.rs), [tracker](https://github.com/Newspicel/sdrmm/blob/de175b45c14ce7cb503c1a0b167aed4638e7979c/crates/dsp/src/radar/track.rs), [assignment](https://github.com/Newspicel/sdrmm/blob/de175b45c14ce7cb503c1a0b167aed4638e7979c/crates/dsp/src/radar/assign.rs), [DAB reference](https://github.com/Newspicel/sdrmm/blob/de175b45c14ce7cb503c1a0b167aed4638e7979c/crates/channels/src/passive_radar/dab_remod.rs), [NLMS](https://github.com/Newspicel/sdrmm/blob/de175b45c14ce7cb503c1a0b167aed4638e7979c/crates/dsp/src/radar/nlms.rs). License: AGPL-3.0-or-later.
- OnAir: [project scope](https://github.com/adswill/OnAir/blob/b7cf9ac054e6e392e30e6c265b5810071644f6f1/README.md), [passive-radar status and design](https://github.com/adswill/OnAir/blob/b7cf9ac054e6e392e30e6c265b5810071644f6f1/docs/passive-radar.md), [implementation](https://github.com/adswill/OnAir/blob/b7cf9ac054e6e392e30e6c265b5810071644f6f1/core/src/radar/passive.cpp), [simulator tests](https://github.com/adswill/OnAir/blob/b7cf9ac054e6e392e30e6c265b5810071644f6f1/tests/radar/test_passive_radar.cpp). License: GPL-3.0.
- VectorWarp: [current CFAR](../src/process/detection/CfarDetector1D.cpp), [current tracker](../src/process/tracker/Tracker.cpp), [OPRS](../oprs), [3lips integration](3LIPS_SETUP.md).
