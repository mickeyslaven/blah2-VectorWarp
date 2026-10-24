# Integration sensitivity shadow

This experiment compares coherent dwell limits and five-channel fusion choices
on the same immutable, strictly admitted IQ without changing the live detector.
The API only writes bounded job descriptions after recorder integrity, sample
containment, and exact-provenance truth checks pass. A separate worker (normally
Strix) performs all replay DSP.

The default matrix is 50, 100, 200, and 500 ms crossed with the deployed
production arm, ideal coherent beam bank, noise-whitened beam bank,
noncoherent fusion, and every physical surveillance channel. A physical
channel is never allowed into its own synthesized reference; this also works
for two-channel receivers, where the other channel becomes the dedicated
reference.

Results contain target-associated detection fractions with explicit evaluated
CPI/time denominators, stable target tracks, total detection cells, nuisance
track count, and wall time per arm. ADS-B supplies only post-decision scoring.
The worker deliberately emits no "winner": method selection requires a frozen,
pass-disjoint holdout with calibrated family-wise false-alarm accounting. Every
result is explicitly non-authoritative and records
`live_processor_work_added_ms: 0`.

After the unchanged detector independently matches a target, one second replay
may export a truth-conditioned pre-fusion complex cutout and reports fixed-cell
noncoherent, trajectory-migrated noncoherent, and common-phase-corrected
coherent accumulation. Phase-increment coherence and phasor concentration make
the measured coherent break point visible. A reference-weight phase proxy is
clearly labelled diagnostic until the native cutout supplies a direct/pilot
phasor. This oracle diagnostic is marked `excluded_from_arm_scoring` and can
never support an independent detection or promotion claim.

Configuration:

```yaml
process:
  detection:
    integration_shadow:
      enabled: true
      queue_path: /var/lib/vectorwarp-api/integration-shadow/queue
      results_path: /var/lib/vectorwarp-api/integration-shadow/results
      receipt_key_path: /etc/vectorwarp/integration-shadow-receipt.key
      dwell_seconds: [0.05, 0.1, 0.2, 0.5]
      fusion_modes: [production, coherent_beam_bank,
        noise_whitened_beam_bank, noncoherent, physical_channels]
      max_pending: 64
      max_wall_seconds_per_clip: 1800
```

Status is read-only at `/api/detection/integration-shadow`. Execute one staged
job with:

```text
python3 script/run-integration-sensitivity-shadow.py \
  --job JOB.job.json --recording STAGED.mchq --admission STAGED.mchq.admission.json \
  --config CONFIG.yml --receipt-key /etc/vectorwarp/integration-shadow-receipt.key \
  --binary blah2 --harness test/integration/processor_replay_harness.py \
  --output JOB.result.json
```

The queue binds the IQ digest, native recorder-metadata digest, exact admission
document, canonical configuration, revision, receiver/config epochs,
receipt-key identity and live producer host into an immutable job identity.
The worker verifies every one,
including RF/sample-rate/channel/gain metadata, atomically claims a job,
enforces a per-clip wall deadline, refuses to run on the live producer host,
uses exclusive atomic result publication, and aborts if the configuration
changes during the run. Result receipts are authenticated with HMAC-SHA-256
using a minimum 256-bit owner-only key copied to the external worker over the
trusted deployment channel; a spool writer cannot forge completion. Job and
result documents also carry portable corruption-detecting hash bindings and
are created mode 0600. Hot queue scans are bounded by archiving older completed
job/result pairs. A result can guide a later held-out experiment,
but cannot promote a method or modify production settings.
