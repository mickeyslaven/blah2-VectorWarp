# Offline resident weak-evidence comparison

`residentWeakEvidenceScore` consumes 1–20 ordered `.vwrps` snapshots with their
sealed `.vwrps.vwrl` capture-lineage sidecars. It emits JSONL to stdout and never
loads a GPU worker, changes detector state, or touches a live service. Build the
`residentWeakEvidenceScore` and `testResidentWeakEvidenceScore` CMake targets in
a configured build with testing or `BLAH2_BUILD_OFFLINE_TOOLS=ON`.

```sh
residentWeakEvidenceScore --anchor 0 frame1.vwrps frame2.vwrps
residentWeakEvidenceScore --anchor 0 --evaluation labels.json frame1.vwrps frame2.vwrps
```

The complete cohort is validated before scoring. Snapshot SHA256, full sealed
snapshot-and-lineage SHA256, running scorer executable SHA256, and optional
evaluation-file SHA256 accompany the results. The processing pass rejects input
changes after validation. Frames must share configuration/source/module/device,
RF and gain identity, and have consecutive logical/generation/consumed sequence
numbers, exact sample lengths, and a constant hop no larger than the CPI.
Missing sidecars, duplicates, gaps and lineage drift fail closed.

The six methods are the exported **single-CPI current power**, the actual
`PostClutterBootstrap` diagonal and fixed-0.75 shrinkage implementations, and the
actual `DirectPathCoherentReplay`/`DirectPathCoherentShadow` 2/4/8-CPI banks.
Current power is already squared and is not squared again. Bootstrap and coherent
map amplitudes are converted to power once. Bootstrap uses guard 2, training 12
per side and diagonal loading 0.001. Coherent scoring fixes equal unit-norm
channel weights, one spatial trial, zero-bin motion, phase direction +1,
pilot coherence at least 0.25 and target coherence at least 0.6. The pilot comes
from the chosen raw anchor channel and recorded reference IQ; evaluation labels
never enter it. These weights are a declared hypothesis, not the recorded
production beam bank or a fitted array manifold.

For each method, local contrast compares the endpoint CUT against the mean of
the same-row training cells outside the two-cell guard, at most 12 cells per
side. Both sides of every pair use the exact intersection of valid background
cells, with at least two cells and a finite positive mean. Coherent warmup and
invalid cells are explicit; raw power is never substituted into a coherent or
bootstrap score. Each method has its own paired support and paired baseline
crossing count. Counts from different supports must not be compared as if their
denominators were equal. The baseline is the endpoint's single CPI, not an
equal-duration noncoherent temporal integrator.

`diagnostic_selected_cells` counts local-contrast crossings at a fixed 10 dB.
This is a map-cell burden diagnostic without NMS, native CFAR, empirical-null
calibration, or track replay. `recorded_native_counts` independently reports the
saved native CFAR/centroid/interpolated flag counts, including the original
native temporal policy; these are not a matched detector comparison against
the alternative statistics. Power values have different units across methods;
use the paired dimensionless contrasts for descriptive comparisons.

An optional manifest supplies exact evaluation-only points, for example:

```json
{
  "version": 1,
  "frames": [{
    "snapshot_and_lineage_sha256": "<64 lowercase hex characters from the frame record>",
    "points": [
      {"kind": "target", "id": "pass-id", "evidence": "independent frozen truth artifact and join", "row": 12, "column": 25},
      {"kind": "control", "id": "control-id", "evidence": "independently adjudicated control artifact", "row": 12, "column": 40}
    ]
  }]
}
```

Indices refer to the decoded display-order Doppler rows and delay columns,
starting at zero. Duplicate/conflicting cells, unknown frame identities and
out-of-map points are rejected. The file is limited to 1 MiB, 20 frame records
and 256 points per frame. Its labels and evidence descriptions must be prepared
independently: the tool binds their bytes to the inputs but cannot establish
their correctness or adjudication. It performs no target peak search and no
truth-based parameter tuning. A run without a manifest emits null evaluation
points. Unmatched map cells are never relabeled as false alarms or controls.
Zero CUT power yields a valid non-selection and null log contrast rather than
nonstandard JSON negative infinity.

Bounds are 512 MiB total snapshot payload, 65,536 cells per frame, at most eight
channels, two million pilot samples, and a cooperative 60-second scoring budget.
Only one snapshot is retained at a time in each pass. There is no batch scheduler
or automatic retry; resource gates remain the caller's responsibility. A failed
run can have earlier frame records on stdout: accept a report only with exit
status zero and its final `complete` record.

All reports explicitly leave PFA, whole-pass detection, false-track delta and
tracking gain unavailable. These diagnostics do not establish a detector winner
or justify promotion. Larger native maps require a separately reviewed increase
of the cell/work budget, not silent cropping.

Focused validation consists of `residentWeakEvidenceScoreCpuContract` and
`residentWeakEvidenceScoreCliContract`. Their tiny synthetic sealed fixtures test
native map layout and power units, rotating pilot phase, exact 2/4/8 warmup,
matched local masks, lineage rejection, JSON output, label hash binding, control
output, and missing/invalid evaluation evidence. Synthetic controls are never
represented as measured real false-track exposure.
