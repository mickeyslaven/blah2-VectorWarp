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
tracker `beam_tolerance` is configured.

`coherent_beam_bank` preserves the calibrated conventional scorer.
`covariance_mvdr` builds a complex spatial covariance from a configurable
delay/Doppler training ring around each candidate, diagonally loads and inverts
that covariance on the GPU, and applies a normalized adaptive matched filter to
the same calibrated manifold. The guard excludes the target neighborhood. If a
candidate covariance is singular, that candidate safely falls back to the
conventional score instead of failing the radar frame.

The publisher reports the circular temporal mean in true-north coordinates,
the current fine-beam bearing, peak separation, circular concentration, sample
count, angular uncertainty, and up to three persistent candidate lobes. It sets
`resolved: true` only after the configured sample minimum and conservative
consistency, uncertainty, and peak-separation gates pass.

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
    scan_beams: 60                # surveillance-beam multiple, at most 127
    candidate_peaks: 3
    guard_radius: 1
    training_radius: 3
    diagonal_loading: 0.01
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

## Interfaces and compatibility

- `/api/tracker` remains the legacy tracker schema.
- `/api/bearing` exposes radar-only temporal bearing evidence.
- `/api/locations` adds bistatic candidate points and uncertainty corridors.
- `/display/locations` overlays gold bearing evidence on orange delay ellipses.
- Disabled or unsupported configurations continue running ordinary passive
  radar without a bearing stream.

The conventional and covariance methods use the already calibrated coherent
array manifold. Experimental neural, MUSIC, and SBL selectors are not included;
they require independent timing and real-aircraft validation before becoming
selectable production methods.
