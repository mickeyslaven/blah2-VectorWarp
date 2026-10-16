# Bearing and bistatic geolocation

VectorWarp can publish an opt-in bearing product when the GPU-resident coherent
beam bank and tracker are active. The ordinary map, detection, tracker, and
Retina payloads are unchanged. Bearing has its own TCP stream and API endpoints.

## Data path

For every accepted delay/Doppler cell, the coherent bank retains the winning
coarse beam used by tracker association and the winning beam from the existing
4× candidate refinement. Candidate cells scan the complete fine-beam ring and
compare the winner with the strongest alternative outside the aperture-limited
main lobe. This avoids treating adjacent interpolation points as independent
directions. Each tracker history observation carries the bounded margin, so
bearing publication uses its temporal median rather than allowing one noisy
frame to resolve or suppress an otherwise consistent track. The additional
work is candidate-only; it does not add another whole-map DSP pass.

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

## Configuration

Bearing is disabled by default. Enabling it requires GPU-resident coherent
tracking, a separate port, and operator-confirmed cable mapping, element
geometry, and true-north orientation:

```yaml
process:
  bearing:
    enable: true
    min_consistent: 3
network:
  ports:
    bearing: 3004
```

The array record must set `mapping_confirmed`, `geometry_confirmed`, and
`orientation_confirmed` to `true`, define `x_axis_bearing_deg_true`, and retain
the measured DAQ-to-element positions. Changing frequency, receiver mapping,
array geometry, or reference synthesis invalidates those confirmations.

## Interfaces and compatibility

- `/api/tracker` remains the legacy tracker schema.
- `/api/bearing` exposes radar-only temporal bearing evidence.
- `/api/locations` adds bistatic candidate points and uncertainty corridors.
- `/display/locations` overlays gold bearing evidence on orange delay ellipses.
- Disabled or unsupported configurations continue running ordinary passive
  radar without a bearing stream.

This first standard implementation uses the already calibrated coherent beam
bank. Experimental neural selectors are not included. Alternative MUSIC/SBL
estimators require independent timing and real-aircraft validation before they
can become selectable production methods.
