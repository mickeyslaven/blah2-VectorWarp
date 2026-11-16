# Offline resident truth labels

`script/map-resident-truth-labels.py` creates the version-1 evaluation manifest
accepted by `residentWeakEvidenceScore`. It labels one explicitly selected
admitted aircraft pass across 1–20 supplied snapshots. Every supplied snapshot
must have supported truth; supply only the intended evaluation endpoints here.
The scorer can separately receive an earlier unlabelled warmup prefix. The
mapper does not infer controls, inspect radar power, search peaks, interpolate
truth, tune a detector, or feed ADS-B to inference.

Build `residentSnapshotGeometry` with the existing offline CMake targets. The
small executable reuses `loadValidatedSnapshot`, `Cohort`, and `frameIdentity`
from the scorer. It exports actual display-order `delayAxis` and `dopplerAxis`
and the sealed snapshot/source/lineage identities, after the same native ABI
validation. It performs no replay or statistic calculation. The helper follows
the scorer's cell, sample and aggregate-byte limits. Both helpers are offline;
normal host resource gates still apply to builds and processing real snapshots.

Example (paths are placeholders, and all inputs must already exist):

```sh
python3 script/map-resident-truth-labels.py \
  --geometry-helper build/residentSnapshotGeometry \
  --admission recording.mchq.admission.json \
  --timeline recording.mchq.timeline.json \
  --metadata recording.mchq.json \
  --profile recording.mchq.replay.json \
  --replay-receipt completed-resident-replay.json \
  --pass-id EXACT_ADMITTED_PASS_ID \
  --output evaluation.json \
  resident-frame-5-5.vwrps resident-frame-6-6.vwrps
```

The script requires the unmodified admission, timeline, recording metadata and
RF profile bytes named by a completed `vectorwarp-iq-protection-replay-v1`
receipt. It binds all their hashes, the mapper/helper hashes, the recording
source hash and each exact snapshot-plus-lineage identity into the output.
The receipt must include the snapshot export hashes and a unique timing record
with the same capture epoch and exact `[first_sample, end_sample)` interval.
The original admission must independently verify the source hash and bind the
metadata bytes. Its selected pass/event/ICAO/capture references must resolve
uniquely; duplicates are rejected even when values happen to agree. Complete
RF profile and single-illuminator geometry are required. JSON inputs are bounded
to 64 MiB each; compressed timelines and multi-illuminator clock conversion are
not supported. Existing output files are never overwritten.

## Signal-time join

`src/blah2.cpp` assigns replay resident frames a synthetic processor-start clock.
That `signalTimestampMs` must **not** be treated as original recording UTC, and
logical-frame ordinal is not a substitute for sample provenance. The mapper
retains the snapshot signal timestamp as identity, then uses the receipt's exact
source-sample correspondence and metadata `eventTime.epochEstimateNs` to derive
the source CPI midpoint in the recording metadata's clock coordinates:

```
source_midpoint_ms = epochEstimateNs / 1e6
                   + (clock_first_sample + clock_end_sample) * 500 / sample_rate_hz
```

For native `MCHT-v1` clips, `RecordingReader` preserves the upstream acquisition
epoch/sample origin; the metadata also carries a separate recorder-local origin
used by `eventTime`. The mapper requires `upstreamTiming.acquisitionEpoch` and
`upstreamTiming.firstSample` to match the native lineage, then translates each
sample by the authenticated offset
`metadata.firstEpochSample - metadata.upstreamTiming.firstSample`. The receipt
must still match the original native sample interval exactly. Both intervals
and the mapping type are retained in the label. Without upstream timing, the
snapshot must directly match the metadata's recorder epoch/sample origin.

The clock's epoch, sample bounds and rate must match the recording metadata,
its full-recording midpoint must agree with both `estimateNs` and the recording
window to within 1 ms, and the snapshot must lie within that recording. A
missing or inconsistent clock/receipt is a hard failure. No timestamps are
invented from filenames or aligned by array position. An epoch zero in legacy
metadata is never silently rebased; only the explicit, hash-bound native MCHT
correspondence can establish a different valid source epoch.

Select the exact or uniquely nearest timeline endpoint to that source midpoint,
then require that *same* endpoint in the selected pass's admitted CPI support.
Never skip an unsupported nearer endpoint to use more convenient truth. The
maximum offset is the smaller of 250 ms and half the admitted CPI duration;
snapshot/admission CPI durations must agree within 1 ms. Equal nearest times,
duplicate endpoints, duplicate snapshot identities, or two snapshots mapped to
one endpoint are rejected.

The original host-arrival clock may explicitly be estimated, unbounded and
non-decision-grade. Those facts are retained in every label's `join` object;
admission does not turn such timestamps into exact RF UTC. The 250 ms threshold
limits the *nominal join residual*, not the original clock's physical error.
These labels support offline map diagnostics only and cannot establish precise
timing, whole-pass detection, false tracks or promotion eligibility.

## Physical cell mapping and rejection thresholds

The selected timeline prediction and admission truth must agree in source ID,
position timestamp (within 1 microsecond), delay (within `1e-6` km) and Doppler
(within `1e-6` Hz). Both must explicitly state
`delay_definition: excess_bistatic_path_km`; each truth RF and snapshot RF must
agree with the recording within 1 Hz. These are provenance/numerical consistency
tolerances, not target-detection gates. No RF rescaling or stale-truth fallback
is applied.

The conversion matches the existing map/track geometry in
`script/score-iq-protection-replay.py`:

```
delay_samples = excess_bistatic_delay_km * 1000 * sample_rate_hz / 299792458
column = unique nearest index in the snapshot's actual delayAxis
row    = unique nearest index in the snapshot's actual dopplerAxis
```

There is no monostatic factor of two and no assumption that a physical delay
bin equals its zero-based column index. Doppler uses the stored display-order
axis, including its actual CPI-derived spacing; it is not FFT-index arithmetic.
Coordinates outside either axis's closed range are rejected, never clamped.
Axes must be strictly increasing and finite. Equidistant bins are rejected
(floating tie tolerance `1e-12` absolute/relative). No local search or neighbour
maximum enters this mapping. Labels retain physical truth and chosen axis values
for audit, while the scorer consumes only the resulting row/column and identity.

## Validation

Run `python3 test/script/test_map_resident_truth_labels.py` for the tiny pure
synthetic suite. CTest's `residentTruthLabelsContract` also supplies the native
geometry and existing sealed-fixture executables. That adds native axis and
identity checks plus missing-sidecar/reordered-cohort rejection. Tests cover
sample/clock mismatch, replay-clock separation, explicit nearest selection,
admission support, pass ambiguity, provenance/hash mismatch, offset axes,
out-of-map truth, nonfinite values, duplicates and ties. Synthetic data provides
no measured false-track exposure.
