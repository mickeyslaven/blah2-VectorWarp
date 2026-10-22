# Cross-fitted HDR replay result

Date: 2026-10-21

The bounded replay-only implementation is
`script/analyze-cross-fitted-hdr.py`.  For every held-out antenna it learns a
winsorized rank-one direct-path mode from the other four antennas on the
opposite slow-time fold.  The held-out samples cannot enter their own
reference.  Four fixed subbands and all five held-out views are combined by a
fixed lower-quartile consensus times agreement fraction; there is no
best-channel or best-subband selection.

The likelihood converts each channel to the physical calibration domain using
its recorded tuner gain, estimates each channel's noise independently, and
propagates held-channel plus synthesized-reference noise through the cross
product.  Clipped samples reduce information instead of being amplified.
Gain is therefore part of a heteroscedastic/censor-aware likelihood, not an
inverse-gain rescaling of the final score.

The corrected clip-trajectory artifacts were used for r277, r94 and r12.  All
translated physical paths were assigned continuous scores and ranks before the
+3 dB label.  The analyzer emits the complete ranked candidate population and
per-view diagnostics; the durable compact comparison, including candidate
counts, is `results/20261021-cross-fitted-hdr-matrix-v1.json`.

## Result

| replay | role | frames | margin over control p99 | target rank | stable cyclic peak |
|---|---|---:|---:|---:|---|
| r277 / N536ME | persistent-track positive | 8 | -1.425 dB | 1813 | no |
| r94 / N237LP | miss | 20 | -1.898 dB | 3905 | no |
| r12 / N4313D | miss | 20 | -2.057 dB | 4064 | no |

The cheap four-window scan of the retained 2.4 MHz IQ found high individual
envelope-periodogram peaks, but their cyclic frequencies did not recur across
windows.  The stability gate therefore prevented cyclic detection for every
replay.

This branch is rejected: it fails to retain the positive control and produces
no miss candidate.  The +3 dB criterion remains a post-scoring validation
label only.  No live RF, receiver configuration, or deployed processing was
changed.
