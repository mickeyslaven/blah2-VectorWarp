# Track-guided two-frame shadow detector

This optional GPU-resident diagnostic tests whether an existing tracker state
could recover a weak moving target by combining the current and previous
500 ms power maps along the track's predicted delay/Doppler path. It does not
change detections, associations, tracks, publication, or CPI cadence while
`promote` is false. With `promote: true`, a guided hit that has no ordinary
associated detection is appended at the track-predicted delay/Doppler cell and
must then survive the ordinary M-of-N, deletion, acceleration,
stationary-clutter, and publication lifecycle.

Enable it only with GPU-resident tracking and `track_before_detect`:

```yaml
process:
  tracker:
    track_guided_shadow:
      enable: true
      # Set only after shadow evidence qualifies the recovery gate.
      promote: false
      max_tracks: 64
      # Optional; defaults below save.path.
      evidence_path: /var/lib/vectorwarp-api/track-guided-shadow-evidence.jsonl
      max_truth_age_s: 3
      max_file_mb: 10
      max_files: 3
```

For each bounded existing track, one GPU invocation:

1. Predicts the current cell from tracker kinematics and maps the previous
   track state to the preceding rolling power map.
2. Averages the two noncoherent powers along that exact path.
3. Applies the same translated path to the range-CFAR training cells.
4. Uses the ordinary two-CPI single-path CFAR threshold. The full-map TBD bank
   pays a multiple-comparison penalty for 27 motion trials; applying that same
   penalty to one tracker-predicted path removes the intended sensitivity gain.
   The shadow therefore spends one pre-registered test per bounded track, while
   M-of-N confirmation, conservative long-history clutter retirement, and the
   kinematic gate remain independent safeguards.
5. Rejects incomplete edge windows and non-finite data. Learned clutter blocks
   only new hypotheses, so an existing moving track can cross a learned cell.

The work is `O(min(active tracks, max_tracks) * CFAR training cells)`, not
`O(range-Doppler map cells)`. Both power maps stay on the GPU. The only host
read is the small bounded result buffer already synchronized with tracker
status. Logs report the separate `gpu_guided_shadow_ms`, aggregate outcomes,
tentative-versus-established evaluation and recovery counts, and bounded
tracker IDs for `guided_would_recover` cases. "Tentative" includes one-hit
hypotheses that have not yet satisfied the configured M-of-N rule, so the
diagnostic measures whether guided recovery can create more confirmed tracks,
not only prolong already-established tracks. A shadow dispatch that exceeds
10 ms latches the diagnostic off for subsequent frames; this cannot stop or
slow the authoritative tracker.

Each unique counterfactual recovery is also attached to the ordinary detection
frame as bounded diagnostic metadata: tracker ID, prior state, predicted delay
bin, bistatic delay in kilometres, and Doppler. The API matches that metadata
against fresh ADS-B delay/Doppler truth and writes rotating JSONL records with
`matched`, `unmatched`, or `ambiguous` classification. Current counters and the
20 most recent records are available at `/api/tracker/guided-shadow`. ADS-B is
an offline label only: it is read after the GPU/tracker decision and is never
an input to detection, association, confirmation, or publication.

The approach follows the established track-before-detect pattern of retaining
pre-threshold evidence, integrating along physically admissible trajectories,
and exploiting a one-step state prediction. Passive-radar research also shows
that longer coherent integration needs explicit range/Doppler migration and
phase compensation. Therefore this first shadow is deliberately noncoherent;
coherent summation must not be enabled until measured inter-CPI phase stability
and a migration-compensated model are validated.

Primary references:

- D. Zheng et al., "Dynamic programming track-before-detect algorithm for
  radar target detection based on polynomial time series prediction," IET
  Radar, Sonar & Navigation, 2016. https://doi.org/10.1049/iet-rsn.2015.0332
- F. Filippini et al., "Experimental Demonstration of Ship Target Detection in
  GNSS-Based Passive Radar Combining Target Motion Compensation and
  Track-before-Detect Strategies," Sensors, 2020.
  https://doi.org/10.3390/s20030599
- L. Zuo et al., "Long-time coherent integration based on intra-partition
  range-Doppler processing for passive bistatic radar," IET Radar, Sonar &
  Navigation, 2021. https://doi.org/10.1049/rsn2.12025
- M. Conti et al., "Motion Compensation for Long Integration Times and DoA
  Processing in Passive Radars," Remote Sensing, 2023.
  https://doi.org/10.3390/rs15041031

## Promotion gates

Keep the feature shadow-only until all of the following hold on replay and live
evidence:

- processor CPI p99 remains below 450 ms for the 500 ms cadence;
- `gpu_guided_shadow_ms` is bounded and capture drops remain flat;
- ADS-B/Yagi counterfactual review shows more true moving-aircraft recoveries
  than stationary or incompatible-path candidates;
- tentative guided recoveries survive ADS-B/Yagi review rather than reinforcing
  false seed hypotheses, with the configured maximum track count remaining a
  hard work and trial bound; and
- authoritative tracker output is byte-identical with the shadow disabled and
  enabled for the same input replay.
