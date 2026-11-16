# Bounded live temporal observer — source candidate, 2026-11-12

The separate `process.detection.live_temporal_shadow` setting connects the
existing `PostClutterBootstrap` statistic to the same portable `TemporalEvidence`
baseline-two and motion-two/four/eight banks for single-lane CPU and native
GPU-resident capture. It is disabled in the generic, USRP and Kraken defaults.
The existing multi-illuminator offline `temporal_shadow` contract is unchanged.
No source change here deploys, enables or promotes the observer.

## Multi-illuminator connection — 2026-11-13

The live observer now also accepts multi-illuminator capture when
`process.detection.live_temporal_shadow.illuminator_id` names exactly one enabled
lane. All other observer limits and the replay-only artifact path remain
unchanged. Only the selected lane creates a worker; it receives that lane's
existing complex post-clutter CAFs before noncoherent fusion. It shares neither
scores nor history with the other lanes. The production detector, PFA and
tracker never consume its output. `track_before_detect` remains unsupported in
the multi-illuminator production path.

The selected lane publishes `liveTemporalShadow` scalar diagnostics alongside
its usual products. Sample intervals in those diagnostics are half-open and
use the original capture sample rate, even when the lane is decimated. The
CAF delay-bin rate is reported separately as `map_sample_rate_hz`; it must not
be substituted for the capture interval rate. The
capture epoch and timestamp come from the matching native publication batch;
the configuration generation advances on lane resets. Unavailable capture
sample positions cannot be replaced by synthetic fallback positions for the
observer. Capture gaps, explicit resets and rejected clutter/reference
CPIs invalidate in-flight products. Backend tests or qualification tooling can
retrieve the single full-precision result slot with `takeLiveTemporalEvidence()`;
no production code consumes it.

An optional numeric `tracker_min_snr_db` on each
`process.multi_illuminator.illuminators[]` entry overrides only that lane's
tracker birth SNR gate. Missing values inherit `process.tracker.initiate.min_snr_db`
or the selected tracker profile. The value must be finite and within -100 to
100 dB, matching the global setting. Each lane reports its effective value and
source in `tracker_birth`. Detector maps, detections, CFAR PFA, M-of-N confirmation
and other lanes remain governed by their existing settings. This field supports
externally calibrated values; setting it is not itself a claim of calibration.

For the first HomePortal measurement, keep all birth thresholds at their
existing values, select `WPXI-RF23`, and use a four-CPI
observer with diagonal bootstrap, zero delay/Doppler motion bins, the existing
524,288-cell cap, a 100 ms work limit and at least 400 ms offer spacing. This is
a bounded performance/lineage qualification setting, not an accepted sensitivity
change. The observer may latch off if the existing 2 ms producer-copy or work
deadline cannot be met; inspect its counters and production cadence before
increasing any algorithmic work. At CPI rates faster than the spacing, dropped
offers deliberately prevent a multi-CPI window from warming. Do not claim a
temporal benefit unless contiguous windows actually mature.

The HomePortal scoring configuration is:

```yaml
process:
  detection:
    live_temporal_shadow:
      enabled: true
      illuminator_id: WPXI-RF23
      bootstrap_covariance: diagonal
      max_cpis: 4
      delay_motion_bins: 0
      doppler_motion_bins: 0
      max_cells: 524288
      max_work_ms: 100
      min_interval_ms: 400
      artifact_path: ""
      artifact_only: false
```

Apply only this nested setting to the existing configuration. Keep all current
lane settings, including the absent WPXI birth override (global 7 dB), intact.
The 301-by-631, one-channel geometry has a conservative scoring memory bound
of 79,023,920 bytes, below the unchanged 256 MiB hard ceiling.

Focused 2026-11-13 source validation in
`/var/tmp/vectorwarp-config-preservation-b7c3b8b`:

- `cmake --build build-hp --target src/blah2.cpp.o -j1` passes with
  C++17, `-Wall -Werror`, including the optional artifact configuration.
- `cmake --build build-hp --target testMultiIlluminatorRadar -j1` passes;
  `bin/test/unit/testMultiIlluminatorRadar 'per-illuminator birth*,selected live temporal*'`
  passes 175 assertions in two cases. It checks unchanged detector products,
  threshold isolation through reset, shadow noninterference, four-CPI warmup,
  discontinuity/reset handling, capture/map-rate distinction, native timestamp
  and capture epoch, and rejection of unavailable acquisition positions.
- `node api/config-manager.test.js` passes 779 device-setting leaf checks;
  `node api/config-settings-matrix.test.js` persists all 267 editable fields.
  `node --check html/js/config_ui.js` and `git diff --check` pass.
- Native builds/tests used one CPU (12), 100% CPU quota, 2 GiB memory maximum,
  no swap, one build job and one BLAS thread. Available memory remained about
  17 GiB. These are source checks, not live performance acceptance or deployment.

Do not derive new birth thresholds from the reported aircraft SNR medians alone.
Freeze per-lane thresholds only after the same fused statistic is evaluated on
independent whole-pass and target-free/nuisance evidence, with operational false
tracks compared at unchanged detector PFA. Temporal promotion still requires
the qualification and explicit authorization described below; this connection
provides no production promotion switch.

## Data and decision boundary

The CPU adapter copies existing complex per-channel post-clutter CAFs. The
resident adapter copies the already-exported native complex-float CAFs, applying
the same channel/delay/Doppler layout transform as the resident startup oracle.
It widens those values exactly into the portable double-precision engine; it
does not invent additional precision. Neither path reads a presentation map.
There is no extra GPU readback, GPU work, resident ABI change, detector call,
tracker call, threshold/PFA edit, or ADS-B input in this observer.

One private full-precision input/result slot retains bootstrap scores,
statistic-family masks, separate temporal banks and their exact CPI lineage.
`takeLatest()` transfers that slot for backend inspection. Radar code never
consumes it for decisions. Timing telemetry contains only bounded scalar
diagnostics, bank readiness and exact decimal-string sample/sequence identity;
presentation-map quantization is unchanged. The optional bounded CAF artifact
below supports offline miss analysis; neither route claims calibrated null
probabilities.

## Exact weak-evidence collection — 2026-11-13

The optional `artifact_path` writes every channel and cell of the post-clutter
CAF on the observer worker. `artifact_only: true` skips bootstrap and temporal
computation, copying the existing resident complex-float surface contiguously
on the radar thread. CPU input retains complex-double precision. Each record
declares its original layout/precision and preserves exact capture epoch,
sequence, half-open sample interval, tuning, processing/configuration epoch,
observer generation and publication timestamp. Original capture sample rate and
CAF delay-bin sample rate are separate mandatory fields, so a decimated lane's
sample positions cannot be misread as its delay geometry. Channel IDs, delay/Doppler axes
and bootstrap/temporal configuration are recorded once in the header. There is
no ADS-B, candidate-selection or threshold input to collection.

The path must be absolute with an existing parent directory. Creation is
exclusive: an existing file is preserved and the export latches off. Default
limits are 512 MiB and 16 frames; configurable limits are 1–1,024 MiB and 1–128
frames. Creation reserves the entire configured byte cap above 128 GiB of free
disk, and every record rechecks the 128 GiB reserve. The existing 2 ms producer
copy cutoff and cooperative worker budget remain in force. Reaching a cap
stops collection; errors disable export, never detection/tracking. Failed or
partial artifacts remain available for inspection and are not deleted.

Use `script/inspect-live-weak-evidence.py ARTIFACT --cell ROW,COLUMN` offline to
validate complete records, compute the artifact SHA-256 and retrieve exact
complex values in displayed delay/Doppler coordinates. The reader rejects
truncation, malformed/oversized payloads, nonfinite CAFs and missing lineage;
it marks capture/generation gaps explicitly. A header-only file contains no
usable observations. Preserve the process binary/config fingerprints and the
paired live detector/tracker publications in a separate collection receipt.
This artifact is not an exact resident detector replay checkpoint: reference
state, fusion weights, detector thresholds and tracker state are absent.

For a first HP capture-only qualification, retain production PFA/tracker gates,
use diagonal covariance with zero motion bins, `max_work_ms: 100` and
`min_interval_ms: 400` overload backoff, with at most eight frames and a unique local artifact
path. Native 601×511×5 complex-float CAFs occupy about 11.7 MiB per frame. This
is a proposed measurement setting, not target-host performance acceptance.
Regular-file writes execute only on the worker, but a blocked filesystem call
can outlast a cooperative deadline or delay its shutdown join; qualify local
storage latency, radar cadence and shutdown before enabling collection.

## Bounds and reset contract

- One background worker and one preallocated input slot; no pending-job queue.
  Producer offers use `try_lock` and drop on pressure or pacing. No producer
  waits for score computation, filesystem writes or worker completion.
- The configured cell ceiling is at most 524,288, with each axis at most 4,096.
  A separate conservative allocation calculation must fit 256 MiB. It includes
  copied CAFs, old/new outputs, eight power histories, masks and row/axis storage.
  No geometry is cropped, quantized or resampled to pass these gates.
- Reported live geometry supplied by the parent review is HomePortal 301×631
  (189,931 cells, one surveillance channel) and HP 601×511 (307,111 cells,
  five surveillance channels). Unit tests verify admission and byte accounting
  for both. This is a memory admission check, not target performance acceptance.
- Offers are spaced at least four times the configured 1–100 ms work budget;
  defaults are 100 ms spacing and 20 ms work. Bootstrap and temporal loops check
  a cooperative deadline every 32 cells. A copy exceeding 2 ms, a worker budget
  failure, invalid numeric input or worker exception latches off the observer.
  A single overrun can occur before the latch: these are cooperative safeguards,
  not hard realtime scheduling guarantees. Allocation, metrics and OS scheduling
  still require target measurement before enabling the setting.
- Exact half-open capture intervals must be contiguous, equal in duration, and
  sequence-consecutive. Epoch, capture-processing generation, tuning or rate
  changes reset the banks. Gaps, duplicates, backpressure/pacing drops, invalid
  sample position and changed statistic-family masks also restart warmup.
  This initial live adapter deliberately admits non-overlapping capture CPIs;
  it cannot silently integrate overlapping or skipped windows.
- Capture generation invalidation suppresses an old completion even when work
  was already in flight. Telemetry never marks a prior result current while a
  newer accepted job is pending. An unavailable status snapshot is explicit.

Native capture already advances its processing epoch on Kraken gain/tuning and
synchronization boundaries. The main connection binds that actual generation;
the worker's channel order and map axes are immutable for its process lifetime.

## Source validation

- Strict C++17 compile of the new observer tests passes with warnings as errors.
- Final Clang ASan/UBSan run, leak detection enabled: 1,910 assertions in 24
  cases pass across `TestLiveTemporalShadow.cpp` and `TestTemporalEvidence.cpp`.
  It covers numerical/bank parity, native CAF layout, unchanged input and
  presentation bytes, drop/gap/identity/mask resets, in-flight invalidation,
  nonfinite rejection, and both target geometries' memory accounting.
- Config-manager/type/boundary tests, persisted-settings matrix (259 editable
  fields), and UI descriptions/device-switch tests pass. `git diff --check`
  passes. Final sanitized executable:
  `/var/tmp/vectorwarp-live-shadow-sanitized-final-20261112`.
- GCC sanitizer linking was unavailable because its installed libasan linker
  target was missing; the completed sanitizer acceptance uses Clang's runtime.

## Required qualification before enabling or promotion

The source remains unqualified for operational use until target-size copies,
cold/warm computation, total radar p99 cadence, capture backlog/drop counters,
memory high-water marks, shutdown and sustained diagnostics load pass on both
actual hosts/platforms. Conservative budgets may latch off at current geometry;
failure must be measured and addressed without weakening production cadence.

Promotion additionally requires exact-IQ comparisons on independently qualified
warmup/calibration/holdout/target-free or nuisance splits, full native weights and
provenance, fixed production thresholds, false-track non-inferiority, portability
and numerical re-review, and explicit live authorization. These diagnostic
scores are neither phase-coherent evidence nor a replacement for the GPU's
optional beam-history/angle gate. No recovered-track or false-alarm benefit is
claimed from source tests.
