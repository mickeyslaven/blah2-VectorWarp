# Open Passive Radar Scorecard (OPRS) v0.2

Status: stable evidence contract for the v1 JSON schemas; reference scorer
version 0.2.0. OPRS is an independent benchmark specification. VectorWarp is
only the first file adapter and is not the definition of the benchmark.

## Purpose

OPRS is a receiver-neutral, channel-count-aware benchmark for passive radar.
It measures whether a system acquires usable IQ, suppresses the illuminator and
clutter, detects targets, forms stable tracks, estimates bearing when capable,
and runs in real time. It is designed for two-channel reference/surveillance
receivers, coherent arrays such as KrakenSDR, and other SDRs that can export
time-aligned baseband IQ.

There is deliberately no weighted overall percentage. The top-level result is a
gate status plus a mission tuple:

`(tracking pass recall, raw-detection pass recall, nuisance tracks/hour, bearing within 30° if applicable)`

This prevents a strong uptime or bearing score from hiding failed detection.

## Capability profiles

Every run declares exactly one profile. Unsupported optional domains are `N/A`,
not zero.

| Profile | Minimum hardware | Required domains | Optional domains |
|---|---|---|---|
| `REF_SURV_2CH` | one reference and one surveillance channel with usable time/frequency synchronization | acquisition, synchronization, reference, cancellation, detection, tracking, runtime | bearing |
| `SURV_ARRAY_NCH` | two or more coherent surveillance elements; synthesized or external reference | acquisition, synchronization, reference, spatial processing, cancellation, detection, tracking, runtime | bearing if geometry/manifold is known |
| `REF_PLUS_ARRAY` | dedicated reference plus two or more coherent surveillance elements | all core domains plus spatial processing | bearing |
| `SINGLE_CHANNEL_REPLAY` | one recorded channel for algorithm/unit testing only | file integrity and deterministic replay | operational detection claims |

The manifest records channel roles (`reference`, `surveillance`, or both), SDR
model only as metadata, ADC/sample format, full-scale convention, centre
frequency, usable bandwidth, sample rate, clock/time synchronization class,
antenna polarization, element coordinates when relevant, and hashes of IQ,
configuration, software, and truth. No Kraken gain index appears in a portable
metric.

## Scorecard domains

### 0. Evaluation-integrity gate

This gate must pass before any performance conclusion:

- immutable input/config/software hashes;
- a fixed, detector-independent opportunity envelope;
- CPI event-time truth with declared freshness and projection;
- unique aircraft-pass identities and no duplicate opportunities;
- complete configuration/receiver/site epoch identity;
- positive and blank controls;
- no tuning on the evaluation partition;
- common-support and effective-sample-size disclosure for adjusted comparisons.

An integrity failure makes the entire run `INVALID`; it does not merely subtract
points.

### 1. Acquisition and RF health

- valid IQ seconds / scheduled seconds;
- frame-gap and dropped-sample rate;
- per-channel rail and near-rail fraction;
- P99.9 and peak magnitude as fraction of full scale;
- per-channel RMS/power imbalance;
- centre-frequency, bandwidth, gain and channel-role readback fidelity;
- inter-channel sample lag, relative phase stability and coherence when the
  declared profile requires coherence.

Telemetry may supply these values, but portable scorers must also derive them
from IQ when possible.

### 2. Reference quality and interference cancellation

- reference occupied-band SNR and temporal stability;
- reference/surveillance coherence by subband;
- direct-path and stationary-clutter suppression in dB;
- target attenuation measured on protected synthetic injections and positives;
- residual noise/clutter floor by delay/Doppler region;
- spatial-null depth and noise gain for array profiles.

Suppression without target preservation is a failure.

### 3. Raw detection

- primary aircraft-pass raw-detection recall at a fixed nuisance budget;
- probability of detection versus injected echo level;
- echo level required for 50% and 90% pass detection (`L50`, `L90`), in dB
  relative to measured reference RMS;
- target margin over the predeclared threshold/control tail;
- false detections per range-Doppler resolution cell and per radar hour;
- breakdown by delay, Doppler, angle when available, bandwidth and background.

### 4. Tracking

- aircraft-pass tracking recall at the same nuisance budget;
- detection-to-track conversion;
- pass-macro continuity and latency;
- track fragmentation (IDs per truth pass);
- unassociated new tracks per valid radar hour;
- wrong associations and track swaps;
- capacity drops, stationary-clutter suppression and initiation diagnostics.

Misses must contribute zero to continuity; reporting continuity only among
successful tracks is an additional diagnostic, never the headline continuity.

### 5. Bearing and spatial estimation

This domain is required only when the profile claims bearing capability:

- pass-macro accuracy within 15°, 30° and 45°;
- median and P90 absolute circular error;
- correct-lobe presence versus correct-lobe selection;
- azimuth-sector and elevation coverage;
- calibration/manifold support, held-out pass count and stability;
- multiple-source and coherent-multipath tests when claimed.

ADS-B or synthetic truth scores bearing but is never an inference input.

### 6. Recoverability and regression safety

- positive-control pass retention;
- validated recovered misses / live misses;
- trial-corrected recovery margin and search family size;
- number of offline candidates, separately and never counted as catches;
- target loss introduced by cancellation, fusion or feature changes.

### 7. Real-time and operational health

- valid processing uptime;
- end-to-end P50/P95/P99 latency;
- CPI processing time / CPI duration;
- queue depth, backpressure and dropped frames;
- CPU/GPU/memory/storage use;
- restart recovery and config/readback consistency.

## Synthetic benchmark layers

Synthetic evidence is first-class but never mislabeled as field performance.

1. **Pure deterministic signal tests** verify math, channel conventions,
   thresholds and reproducibility.
2. **Injection into real background IQ** measures sensitivity against actual
   direct path, noise, clutter, site reflections and receiver artifacts.
3. **Held-out real aircraft passes** establish operational recall and bearing.

Every synthetic run includes a no-injection blank using the identical background
and method. The same injected samples and random seed are replayed through every
candidate. Training backgrounds, parameter-selection backgrounds and evaluation
backgrounds are disjoint by recording session.

The portable injection truth is a time-indexed trajectory, not only one constant
cell: amplitude relative to reference RMS, bistatic delay, Doppler, azimuth and
elevation when supported. Optional fields describe acceleration, amplitude
scintillation, multipath copies and multiple targets.

For `REF_SURV_2CH`, the reference channel remains unchanged and a delayed,
Doppler-shifted copy of it is injected only into surveillance. For array
profiles, surveillance injection uses the declared ideal or measured manifold.
The result must state which. Injection into every channel's own waveform is a
different all-surveillance-array test and cannot be reported as a dedicated
reference/surveillance benchmark.

The initial conformance grid spans:

- several hashed real backgrounds and a pure-noise deterministic background;
- core and extended delay regions;
- clutter-band, near-clutter and clear Doppler;
- both Doppler signs;
- full azimuth coverage for bearing-capable profiles;
- an echo-level staircase bracketing failure through reliable acquisition;
- single target, crossing targets, and one controlled multipath case.

Report detection/tracking probability curves and `L50/L90`; do not rank methods
from one injected amplitude. Physical-unit resolution and the number of
independent backgrounds are always shown.

## Real-traffic adjustment

See `OPERATIONAL_SUCCESS_SCORECARD.md`. Same-IQ paired replay is preferred.
Live comparisons use predeclared interleaved blocks when safe, raw results, and
direct standardization or matching to a frozen reference traffic distribution.
No geometry adjustment can rescue poor common support or a small effective
sample size.

## Status model

Each domain is `PASS`, `WARN`, `FAIL`, `INVALID`, or `N/A`, and includes the raw
numerator, denominator, units, interval, evidence hashes, and threshold source.
The overall state is:

- `INVALID` if evaluation integrity fails;
- `FAIL` if any required safety/real-time/RF gate fails;
- `NOT_QUALIFIED` if required evidence is absent or underpowered;
- `QUALIFIED` only when every required domain passes;
- `IMPROVED` only when the candidate beats the baseline on mission recall at a
  non-inferior nuisance burden with no required-domain regression.

This status model is shared by two-channel and array systems. Capability claims,
not channel count or brand, determine which optional domains become required.

## Normative anti-gaming contract

Terms **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are normative.

1. The evaluator MUST freeze and hash an `oprs-evaluation-plan-v1` before an
   evaluation run. Its physical delay/Doppler envelope is independent of the
   detector configuration. A system may decline to emit a result, but that
   opportunity remains in the denominator and scores as a miss.
2. Every opportunity is the unique tuple `(plan, pass_id, cpi_id,
   event_time_ns)`. Duplicate IDs, duplicate pass/CPI truth, unknown
   opportunities, changed CPI timestamps, missing epochs, non-finite values, or
   cardinalities that cannot be reconstructed from individual rows invalidate
   the evidence. Aggregate success counters are never accepted as primary data.
3. Truth MUST be interpolated or projected to CPI event time within the frozen
   tolerance. The trial MUST attest `inference_truth_access=false`. ADS-B and
   synthetic truth may score output but MUST NOT enter detection, tracking,
   cancellation, beam selection, or thresholding.
4. A scored track CPI requires a stable `track_id`, confirmed state,
   `measurement_supported=true`, and a unique `measurement_id`. Coasting is a
   diagnostic and does not count as a measurement-supported track CPI. This
   permits a legitimate internal tracker measurement below the public raw-
   detection reporting threshold without accepting an unexplained track bit.
5. Pass recall is pass-macro. Continuity is `measurement-supported confirmed
   track CPIs / all planned eligible CPIs` per pass and then averaged over all
   passes; missed passes therefore contribute zero. Fragmentation counts unique
   qualifying track IDs per truth pass.
6. Nuisance burden is new unassociated tracks per valid radar hour. Recall is
   never accepted without nuisance evidence from the same run and envelope.
7. Same-IQ paired replay is the primary comparison design. Live comparisons
   MUST use predeclared interleaved blocks where operationally safe. Adjusted
   comparisons MUST publish overlap exclusions, weights, maximum weight,
   effective sample size, cluster-resampled uncertainty, and raw results. OPRS
   rejects an adjusted superiority claim if common support or effective sample
   size was not predeclared and met.
8. Training, selection, and evaluation recording sessions MUST be disjoint.
   Hashes cover background IQ, system/config, code, truth, scenarios, and seeds.
   A blank control accompanies each injection background.

An integrity violation yields `INVALID`, not `FAIL`: the performance value is
unknown. Missing but otherwise valid evidence yields `NOT_QUALIFIED`.

## Metric definitions and statistical policy

All metric thresholds are mission-class policy and are frozen in the plan; OPRS
does not hide application tradeoffs in a universal weighted score.

- Proportions include numerator, denominator, and two-sided 95% Wilson interval.
- Operational recall uses an aircraft pass as the independent unit. CPI counts
  are diagnostics, not pseudoreplicated confidence samples.
- Detection and tracking sensitivity use probability by injected level in dB
  relative to measured reference RMS. `L50` and `L90` are monotone-interpolated
  crossing levels. If the staircase does not bracket a crossing, report a
  one-sided bound rather than extrapolating.
- Multi-background injection uncertainty resamples recording session first,
  then target within session. A single background cannot qualify a general
  sensitivity claim.
- Bearing is pass-macro within 15/30/45 degrees plus sample median/P90 absolute
  circular error. Correct-lobe presence and selected-lobe correctness are
  separate. Results are stratified by azimuth sector and elevation.
- Offline recovery requires a held-out miss, positive controls, a frozen search
  family, and Holm family-wise correction across attempted candidates. An
  uncorrected offline candidate is never a recovered aircraft.
- RF health is derived from normalized IQ where possible: valid seconds,
  clipping/near-rail fractions, P99.9/peak magnitude, RMS balance, lag,
  coherence, and phase stability. Vendor gain numbers remain provenance only.
- Target preservation is a hard safety gate. Cancellation/null-depth gains
  cannot compensate for unacceptable injected-target or positive-control loss.

## Evidence-layer claim labels

Every final scorecard has exactly one `claim_scope`:

| Claim scope | What it establishes | What it cannot establish |
|---|---|---|
| `pure_synthetic` | conventions, math, determinism, known-signal behavior | site performance |
| `background_injection` | sensitivity in specific hashed RF backgrounds | real-aircraft operational recall |
| `held_out_real` | operational validity for the declared site/epochs/traffic support | absolute sensitivity below observed traffic |

Separate scorecards may be shown together, but their observations MUST NOT be
pooled into one numerator or overall status.

## Versioned artifacts

- `schemas/oprs/system-manifest-v1.schema.json`: receiver/capability contract.
- `schemas/oprs/benchmark-scenario-v1.schema.json`: trajectory and injection.
- `schemas/oprs/evaluation-plan-v1.schema.json`: frozen denominator and truth.
- `schemas/oprs/trial-output-v1.schema.json`: system-under-test output.
- `schemas/oprs/scorecard-v1.schema.json`: final domain status and mission tuple.
- `oprs/`: dependency-free reference validator, scorer, injector, and IQ health
  implementation.
- `script/oprs`: portable command entry point.

Semantic validation is stricter than JSON Schema because uniqueness, profile
cardinality, hashes, event-time identity, and cross-file consistency require
cross-record checks.
