# Measured installed-site interference null trial — 2026-10-22

## Hypothesis

The elevated five-element array is surrounded by a nearby brick wall, roof
plane, metal chimney cap, cables, and neighboring structures. The installed
direct response is stable and approximately rank one, but differs substantially
from the ideal free-space manifold. A shallow null on the measured dominant
composite spatial mode may therefore suppress direct-plus-site reflection
contamination without guessing a reflector azimuth from photographs.

The operator-provided labeled photograph confirms the five physical elements,
but camera perspective is not used as electrical azimuth evidence. The measured
eigenmode supplies the interference constraint.

## Replay result

Three exact-provenance positive clips from two independent aircraft passes were
replayed at fixed 3, 6, 10, and 15 dB composite-null depths, followed by
noise-whitened fusion at 3, 6, and 10 dB. Four separately labeled overlap misses
in the same IQ clips were also scored.

| Arm | Target frames | Detection cells | Track IDs | Positive clips tracked |
|---|---:|---:|---:|---:|
| Production coherent, 0 dB | 11 | 1,792 | 160 | 3/3 |
| Noise-whitened, 0 dB | 15 | 1,913 | 163 | 3/3 |
| Coherent, 6 dB null | 11 | 1,797 | 158 | 3/3 |
| Noise-whitened, 3 dB null | 16 | 1,888 | 145 | 3/3 |
| **Noise-whitened, 6 dB null** | **16** | **1,870** | **146** | **3/3** |
| Noise-whitened, 10 dB null | 15 | 1,866 | 145 | 3/3 |

The 6 dB combination increased truth-associated target frames by 5/11 (45%)
relative to production coherent fusion, while increasing total detection cells
by 78/1,792 (4.4%) and reducing observed track-ID churn by 14/160 (8.8%). Versus
whitening alone it added one target frame, removed 43 detection cells, and
removed 17 track IDs. All positive tracks were retained. No overlap miss was
recovered at any depth.

This is promising positive-control evidence, not a validated aircraft recovery:
only two independent passes are represented.

## Live bounded trial

The user explicitly authorized a live attempt. At approximately 16:19 EDT the
processor began a bounded trial with:

- config SHA `80aea56b3d16510aa3aa46521241c4f4c648236b8220a3b94308c35c9c236f22`;
- `noise_whitened_beam_bank`;
- measured composite `direct_path_null_db: 6`;
- unchanged 563 MHz, 2.4 MS/s, five coherent channels, mapping, and gains
  `[19.7,25.4,20.7,25.4,22.9]`.

The prior config SHA is
`ea5ddd10b6a717716ad1f44ad069fc1cb4f63172f8041f5ba247fd6f1783ff15`
with coherent fusion and a zero-depth null. The config-store backup is the
rollback source. The Kraken receiver readback was `already-matched`; no retune
or gain command was sent. After revision reconciliation, Heimdall, API, and
processor were active and frames advanced.

The same receiver readback showed zero rail and near-rail components on every
channel. P99.9 fractions were 0.522, 0.475, 0.459, 0.373, and 0.537. This is
evidence against current ADC clipping; it does not exclude strong but linear
spatial interference or analog behavior not represented by rail telemetry.

The hourly heartbeat now monitors trial-vs-baseline finalized eligible passes,
raw detections, track formation, clutter burden, stationary suppression,
continuity, latency, and rollback conditions without combining config epochs.
