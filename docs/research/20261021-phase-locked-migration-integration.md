# Phase-locked, migration-compensated complex CAF integration

`script/analyze-phase-locked-migration.py` is an offline/replay-only analysis
arm for the frozen 563 MHz matrix.  It follows the exact per-CPI ADS-B
trajectory in the complex CAF cutout, coherently integrates a fixed beam after
removing a per-CPI common pilot phase, and evaluates every translated trajectory
as a matched control.

The producer-facing output is compatible with
`stack-current-epoch-evidence.py`: it has one `methods.phase_locked_migration`
entry and an aligned `score_vectors.phase_locked_migration` control vector.
The raw score at each target/control is the maximum across the declared beam,
phase orientation, residual delay/Doppler bin grid, and phase-quality gate.
That same maximum is applied to controls, so those choices are charged by the
empirical calibration; their counts are also persisted as
`parameter_trial_count` and `internal_search_trials_per_path`.

The replay cutout producer now writes `pilot_reference`: a normalized,
pre-cancellation direct-path/reference correlation formed over each CPI.  It
combines each surveillance/reference correlation using the measured array
reference weights (or uses the dedicated reference path), so it is a physical
direct-path phase observation rather than the old beam-weight proxy.  The
preferred input is therefore `pilot_reference`; `direct_path_reference` and
`reference_component` remain accepted for compatible experimental producers.
Old v1 cutouts contain only `reference_weights`; the CLI's automatic fallback
labels that value `reference_weight_proxy`.  The matrix suite refuses that
fallback as `diagnostic_proxy_not_qualified`, so it cannot silently join a
stack or acceptance result.  A phase-increment coherence gate rejects
recordings with unstable pilot increments before any score is emitted.

Example on CASS after the existing spatial replay has retained its replay JSON:

```sh
$PYTHON script/analyze-phase-locked-migration.py \
  --replay /home/mickeyslaven/wwat563-spatial-results/HEX-rREV-replay.json \
  --config /home/mickeyslaven/wwat-current-config.json \
  --job /home/mickeyslaven/wwat563-spatial-results/HEX-rREV-job.json \
  --output /home/mickeyslaven/wwat563-results/phase-vectors/HEX-rREV.json \
  --phase-source auto --phase-gates 0.6 --delay-residual-bins 0 \
  --doppler-residual-bins 0
```

No output is a recovery claim.  Acceptance still requires an independent
positive retained, a persistent truth-near miss path, and the existing
trial-corrected +3 dB-equivalent evidence margin after this vector joins the
stacker.
