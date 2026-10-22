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

The preferred input is a per-frame complex `direct_path_reference`,
`pilot_reference`, or `reference_component` added by a replay.  Current v1
complex cutouts contain only `reference_weights`; the CLI's automatic fallback
uses the first nonzero weight solely as `reference_weight_proxy` and labels it
as such.  It must not be interpreted as evidence that direct-path phase was
measured.  A phase-increment coherence gate rejects recordings with unstable
pilot increments before any score is emitted.

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
