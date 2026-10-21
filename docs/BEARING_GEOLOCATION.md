# Bearing and bistatic geolocation

VectorWarp can publish an opt-in bearing product when the GPU-resident coherent
beam bank and tracker are active. The ordinary map, detection, tracker, and
Retina payloads are unchanged. Bearing has its own TCP stream and API endpoints.

## Data path

For every accepted delay/Doppler cell, the coherent bank retains the winning
coarse beam used by tracker association. The bearing-only candidate scan then
retains three aperture-separated lobes and their quantized relative powers in
each tracker observation. Temporal publication accumulates all three rather
than pretending that different single-frame winners are the only ambiguity.
The additional work is candidate-only; it does not add another whole-map DSP
pass or alter CFAR scores, legacy track JSON, or Retina output. As with the
legacy refined winner, it participates in association only when the optional
tracker `beam_tolerance` is configured. Association compares every retained
lobe on both observations, so a temporarily wrong primary lobe does not discard
an otherwise valid range/Doppler track.

`coherent_beam_bank` preserves the calibrated conventional scorer.
`covariance_mvdr` builds a complex spatial covariance from a configurable
delay/Doppler training ring around each candidate, diagonally loads and inverts
that covariance on the GPU, and applies a normalized adaptive matched filter to
the same calibrated manifold. The guard excludes the target neighborhood. If a
candidate covariance is singular, that candidate safely falls back to the
conventional score instead of failing the radar frame.

When `max_rate_deg_s` is positive, the publisher runs a multi-hypothesis path
search through the retained lobes and rejects inter-frame jumps faster than the
configured aircraft angular-rate limit. It reports the selected path's bearing
rate and keeps unresolved alternatives visible. A zero value preserves the
legacy temporal histogram exactly.

The publisher reports the circular temporal mean in true-north coordinates,
the current fine-beam bearing, peak separation, circular concentration, sample
count, angular uncertainty, and up to three persistent candidate lobes. It sets
`resolved: true` only after the configured sample minimum and conservative
consistency, uncertainty, and peak-separation gates pass.

When the tracker persistent-clutter option is enabled, the bearing record also
contains `stationary_clutter` and `publishable`. A history must stay within 1.5
delay and Doppler bins for the configured persistence interval before it is
classified as stationary. The raw `/api/bearing` and `/api/locations` records
remain available for diagnosis, but stationary records are never marked
resolved and `/display/locations` hides them by default. Add
`?showStationary=1` to inspect them.

The nominal fine scan spacing is not physical accuracy. Aperture size,
multipath, sidelobes, calibration error, and target geometry can make the real
uncertainty much wider. Consumers must honor `resolved` and `uncertainty_deg`.

## Geolocation

`/api/locations` intersects each radar-only bearing candidate with its bistatic
delay surface. For receiver-to-transmitter baseline `d`, excess path `rho`, and
angle `theta` between the transmitter bearing and candidate target bearing:

```text
r = rho (2 d + rho) / (2 (d + rho - d cos(theta)))
```

Resolved results include a point and an uncertainty corridor obtained from the
bearing limits. Uncertainty is never allowed below an aperture-derived
half-power half-width: the 4x fine scan locates a beam maximum but is not treated
as the array's physical angular resolution. Unresolved results include candidate
points only. The map keeps the full delay ellipse visible to distinguish
delay-only evidence from a bearing-constrained estimate.

The API also retains a short, radar-only hypothesis history for each track.
As the aircraft moves, every candidate bearing intersects a different delay
ellipse; a dynamic-programming pass favors paths with physically plausible
ground speed and acceleration. The diagnostic `estimate` therefore improves
across scans in the same way that changing geometry improves a mobile direction
finder, except here the target supplies the motion while both radar sites stay
fixed. This smoother never upgrades an unresolved radar bearing to a strict
`location`; it reports `geometry_consistent_samples` and leaves alternatives
available for diagnosis.

ADS-B is not an inference input. It may be displayed after the radar result for
validation, but cannot select a lobe, improve consistency, resolve a point, or
change geometry acceptance.

`min_peak_separation_db` is the minimum temporal power margin between
the winning lobe and the strongest aperture-separated alternative. The default
3 dB requires the winning lobe to carry at least twice the alternative's power;
weaker margins remain useful candidate bearings but are not labelled resolved.

## Configuration

Bearing is disabled by default. Enabling it requires GPU-resident coherent
tracking, a separate port, and operator-confirmed cable mapping, element
geometry, and true-north orientation:

```yaml
process:
  bearing:
    enable: true
    method: covariance_mvdr       # coherent_beam_bank remains the default
    reference_anchor: live_eigenbeam # fixed_site rejects live anchor wander
    scan_beams: 60                # surveillance-beam multiple, at most 127
    candidate_peaks: 3
    guard_radius: 1
    training_radius: 3
    diagonal_loading: 0.01
    max_rate_deg_s: 20            # 0 keeps legacy histogram selection
    manifold_file: /etc/vectorwarp/bearing-manifold.json # optional
    channel_mapping: [0, 1, 2, 3, 4] # optional bearing-only permutation
    min_consistent: 3
    min_peak_separation_db: 3
network:
  ports:
    bearing: 3004
```

The array record must set `mapping_confirmed`, `geometry_confirmed`, and
`orientation_confirmed` to `true`, define `x_axis_bearing_deg_true`, and retain
the measured DAQ-to-element positions. Changing frequency, receiver mapping,
array geometry, or reference synthesis invalidates those confirmations.

`channel_mapping` maps each logical surveillance-map slot to a physical regular-
polygon element index. It must be a complete permutation. It affects only the
bearing refinement manifold, making suspected cable/order mappings safe to
compare while the established detection and Retina paths remain fixed.

`reference_anchor` controls only the fine bearing refinement bank. The default,
`live_eigenbeam`, preserves the adaptive reference response. `fixed_site` uses
the measured manifold response at the configured direct-path angle, or the
ideal array response when no measured manifold is loaded. It does not alter
reference synthesis, clutter cancellation, detection maps, tracking, or Retina
publication. At 2.4 MSPS a delay sample spans about 125 m one-way, so ordinary
roof multipath cannot be isolated by selecting an earlier delay sample; an
absolute site/manifold response is the appropriate constraint.

The recorder rejects stale ADS-B truth, ambiguous delay/Doppler assignments,
and elevations above `max_elevation_deg`. A planar azimuth manifold cannot fold
near-overhead arrivals into the same steering row without biasing every bearing;
those samples remain available to the radar and tracker but are excluded from
site calibration.

`manifold_file` replaces only the bearing refinement steering vectors; the
established detection beam bank remains unchanged. It is a strict JSON file:

```json
{
  "schema": "vectorwarp-bearing-manifold-v1",
  "frequency_hz": 539000000,
  "responses": [
    [[1.0, 0.0], [0.8, 0.2], [0.7, -0.1], [0.9, 0.1], [0.8, -0.2]]
  ]
}
```

`responses` must contain exactly `scan_beams` rows in local counter-clockwise
order and one nonzero `[real, imaginary]` complex response per logical bearing
channel. The file frequency must match the capture frequency. VectorWarp ratios
each direction against the measured direct-path direction, then applies the
live Heimdall/direct-path channel calibration. This preserves drift correction
while compensating direction-dependent antenna, coupling, mounting, and roof
effects. Invalid files stop startup rather than falling back silently.

## Built-in installed-array recorder

VectorWarp can learn the response in its actual installation without a
site-specific external recorder. Collection starts automatically when coherent
Kraken detection, confirmed geometry/orientation, and ADS-B truth are available;
`enabled: false` remains an explicit opt-out. It publishes only the complex surveillance-channel values at accepted radar cells,
matches them to fresh aircraft from VectorWarp's configured ADS-B source, and
appends bounded, rotating JSONL. ADS-B supplies offline labels only; it never
enters detection, tracking, bearing inference, or Retina output.

The recorder requires a Kraken receiver, coherent fusion, detection, confirmed
mapping/geometry/orientation, and enabled ADS-B. Collection works on both CPU
and GPU-resident coherent processing paths:

```yaml
process:
  bearing:
    manifold_recorder:
      enabled: auto
      path: /var/lib/vectorwarp-api/calibration/manifold-samples.jsonl
      min_snr_db: 6
      min_elevation_deg: 0
      max_elevation_deg: 15
      max_delay_error_km: 0.25
      max_doppler_error_hz: 5
      max_truth_age_s: 3
      max_file_mb: 64
      max_files: 4
network:
  ports:
    manifold: 3005
```

Recorder health and counts are available at
`/api/bearing/manifold-recorder`. A slow truth source cannot back-pressure the
radar: the API keeps at most the newest evidence frame while matching or writing.
Full ambiguity maps are never copied or stored.

### Automatic measured-manifold lifecycle

The normal Kraken configuration also runs the fitter and validation lifecycle
in a background worker. No calibration script or frame-count judgement is
required. `/api/bearing/manifold-automation` reports `collecting`, `evaluating`,
`qualified-pending-restart`, or `active`, together with every unmet gate. The
worker writes `<path>.candidate.json`; it promotes only a qualified model to
`<path>.qualified.json`.

Promotion is deliberately pass-balanced. Frames from the same aircraft pass
share one vote, and complete passes are withheld from fitting. The candidate
and VectorWarp's default free-space model are replayed on exactly those unseen
passes. The built-in gates require, by default:

- at least 40 independent passes and 12 aircraft;
- independent coverage in at least six of eight azimuth sectors, with five
  passes and three aircraft in every counted sector;
- at least 12 withheld passes across four sectors;
- a measured-model median error no worse than 45 degrees;
- at least a five-degree or 15-percent median improvement over the default;
- no reduction in the withheld within-45-degree rate; and
- no independently tested sector regressing by more than eight degrees.

When all gates pass, `manifold_automation.auto_promote: true` atomically selects
the qualified file and requests the configured controlled service restart. If
restart control is unavailable, the selection is saved and activates at the
next ordinary restart. A site-defining setting change automatically removes an
auto-selected model before saving the new configuration, so the recorder starts
a new corpus. `auto_promote: false` retains automatic collection, fitting, and
shadow comparison but leaves activation to the operator.

ADS-B is used only to label the calibration corpus and score the shadow A/B
test. It is not available to live bearing, detection, tracking, or Retina after
promotion. Fit work runs outside the radar processor and does not consume its
CPI budget.

The recorder also writes `<path>.reference.jsonl` whenever the array-eigenbeam
reference analysis advances. This bounded shadow log stores the coherent
fraction/gain and each channel's complex principal-eigenvector response relative
to channel 0 (or the strongest channel if channel 0 is unusable), together with
the maximum phase and amplitude step since the preceding update. It does not
alter calibration, bearing, detection, tracking, or Retina output. Use it to
distinguish time-varying interchannel drift from a stable installed-site
manifold: stable reference phase with repeatable wrong lobes points to mounting
or multipath, while phase motion correlated with bearing errors supports a
future, separately qualified correction. Site fingerprint changes archive this
log and begin a clean collection alongside the manifold samples.

When covariance MVDR is active, each accepted sample also stores the small
per-target loaded spatial covariance used by the live scorer. This is bounded by
eight channels and the already accepted sparse detections; it does not retain a
full ambiguity map. The covariance makes the installed-array wiring check a
replay of the actual complex channels and MVDR equation instead of a rotation of
the finished bearing.

The recorder fingerprints receiver/transmitter sites, frequency, sample rate,
channel mapping, and complete array geometry. A changed fingerprint archives the
previous active JSONL as `.site-previous` (within the configured file bound),
writes a new site ledger, and begins a clean collection. Samples from different
installations are therefore never silently fitted as one manifold.

For offline experiments or manual review, the same fitter remains available:

```sh
node script/fit-bearing-manifold.js \
  --input /var/lib/vectorwarp-api/calibration/manifold-samples.jsonl.3 \
  --input /var/lib/vectorwarp-api/calibration/manifold-samples.jsonl.2 \
  --input /var/lib/vectorwarp-api/calibration/manifold-samples.jsonl.1 \
  --input /var/lib/vectorwarp-api/calibration/manifold-samples.jsonl \
  --output /etc/vectorwarp/bearing-manifold.json \
  --beams 60 --min-samples 20
```

The fitter removes each echo's arbitrary common complex scalar, robustly rejects
inconsistent samples, reserves holdout observations, refuses mixed site/config
fingerprints, requires useful circular coverage, and labels interpolated bins.
The live eigenbeam reference weights anchor the measured direct-path manifold
bin used by VectorWarp's drift correction. The file records coverage, elevation
range, and holdout correlation metadata. Review
those diagnostics before setting `manifold_file`; an azimuth-only model does not
erase elevation dependence or roof multipath outside the observations.

## Built-in channel-order and heading diagnostic

`/api/bearing/diagnostic` and the Radar locations page expose a read-only,
shadow-only diagnostic once replay-capable samples exist. The same check can be
run without the web service:

```sh
node script/evaluate-bearing-calibration.js \
  --config /etc/vectorwarp/config.yml \
  --input /var/lib/vectorwarp-api/calibration/manifold-samples.jsonl
```

The diagnostic replays the recorded complex target vector and its local loaded
covariance through every cyclic and reflected regular-polygon channel mapping.
It reports pass-balanced median error, aircraft and azimuth-sector coverage,
the currently configured mapping, the best replay mapping, and a separate
heading-only correction. It never edits configuration or feeds ADS-B into live
bearing, detection, tracking, or Retina output.

The same endpoint also scores the recorded direct-path eigenbeam against the
configured transmitter as a known-angle RF source. It reports phase residuals
for the configured mapping and heading, every regular-polygon mapping at the
configured heading, and every whole-degree heading with the configured mapping.
This closes an important observability gap in `fixed_site`: that setting builds
the bearing anchor from configured geometry, while the known-angle diagnostic
uses the independently observed eigenbeam phases. Reference amplitude imbalance,
coherence, multipath, and mapping/heading aliases remain visible, and the result
is always shadow-only; it never edits configuration.

For a stronger check, the transmitter-agnostic survey CLI combines two or more
known RF sources. Each manifest source supplies its name, latitude, longitude,
frequency, and reference JSONL path; no station list or site-specific bearing is
built into VectorWarp. Sources must provide at least 10 updates, median coherence
of at least 0.8, and median inter-channel amplitude span no greater than 6 dB.

```sh
node script/evaluate-reference-survey.js \
  --config /etc/vectorwarp/config.yml \
  --manifest reference-survey.json
```

```json
{"sources":[
  {"name":"North reference","latitude":40.1,"longitude":-80.0,
   "frequency_hz":527000000,"reference":"north.reference.jsonl"},
  {"name":"East reference","latitude":40.0,"longitude":-79.9,
   "frequency_hz":485000000,"reference":"east.reference.jsonl"}
]}
```

The output ranks the configured mapping and the best forward and reverse
handedness families over every whole-degree fitted phase-axis heading. The
physical CH0/+X heading remains configuration truth: a fitted phase-axis offset
is calibration evidence, not an assertion that the array physically moved.
The tool is read-only and never applies its result.

A symmetric regular polygon has an unavoidable identifiability limit: rotating
the channel labels by one element is mathematically equivalent to changing the
array heading by one element angle. Therefore even strong replay evidence is a
review result, not proof of mislabeled cables. A known-angle RF source or a
physical one-channel-at-a-time verification is required to distinguish those
two explanations. Older v1 samples remain valid for manifold fitting but lack
the covariance needed for exact MVDR mapping replay.

When track-before-detect and `tracker.beam_tolerance` are both enabled, the
multi-CPI integrator also requires coarse-angle consistency along each
delay/Doppler motion trajectory. This is joint delay-Doppler-angle TBD; omitting
`beam_tolerance` retains the prior delay/Doppler-only integrator.

## Interfaces and compatibility

- `/api/tracker` remains the legacy tracker schema.
- `/api/bearing` exposes radar-only temporal bearing evidence.
- `/api/bearing/diagnostic` reports the shadow-only wiring/heading replay.
- `/api/locations` adds bistatic candidate points and uncertainty corridors.
- `/display/locations` overlays gold bearing evidence on orange delay ellipses.
- Disabled or unsupported configurations continue running ordinary passive
  radar without a bearing stream.

The conventional and covariance methods use the already calibrated coherent
array manifold. Experimental neural, MUSIC, and SBL selectors are not included;
they require independent timing and real-aircraft validation before becoming
selectable production methods.

## Frequency-resolved coherent-multipath shadow

A nearby coherent reflector can occupy the same delay sample as the direct
transmitter. Whole-band covariance then sees one rank-one spatial mode even
though the path delay remains visible as phase slope across RF frequency. The
optional frequency-resolved shadow preserves that information without changing
reference synthesis, detection, tracking, bearing, or Retina output:

```yaml
process:
  bearing:
    frequency_resolved_shadow:
      enabled: true
      fft_size: 2048
      subbands: 24
      bandwidth_fraction: 0.9
      diagonal_loading: 0.01
      capture_seconds: 0.25
      interval_s: 300
      timeout_ms: 15000
      path: /var/lib/vectorwarp-api/calibration/frequency-resolved-reference.json
```

The API launches a bounded worker that attaches as another read-only Heimdall
MCHQ client. It derives frequency, sample rate, channel count, channel order,
element coordinates, array orientation, and transmitter direction from the
active configuration. There are no channel, frequency, television-standard,
site, or transmitter-name constants.

Each run accumulates complex cross-spectral matrices across the configured
subbands, extracts their principal coherent responses, measures rank-one
fraction and phase curvature, and constructs diagonally loaded direct-
constrained MVDR candidate weights. Candidate weights use the mathematical
convention `y = w^H x`. They are diagnostic output only; the processor does not
load or apply them. The output is atomically replaced, carries a fingerprint of
the complete RF/geometry model, and is exposed at
`/api/bearing/frequency-resolved-shadow`.

The same bounded analysis can run as a one-shot during staged deployments
without changing the active API configuration:

```sh
node script/run-frequency-resolved-shadow.js \
  --config-url http://receiver-host:3000/api/config \
  --output /var/lib/vectorwarp-api/calibration/frequency-resolved-reference.json
```

Packaged `vectorwarp-frequency-resolved-shadow.service` and `.timer` units run
that one-shot with least privilege every five minutes. Sites whose API is not
on loopback can override `VECTORWARP_CONFIG_URL` in
`/etc/vectorwarp/frequency-resolved-shadow.env`.

Promotion requires a separate processor implementation and held-out ADS-B
comparison across multiple aircraft and sectors. In particular, a well-
conditioned direct constraint is not by itself proof that suppressing the
reference composite improves aircraft bearings.
