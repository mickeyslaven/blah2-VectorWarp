# Two-site geographic fusion review — 2026-11-15

Source-only implementation. No live configuration, services, model promotion,
commit, push, or deployment is part of this change.

## Verified geometry and useful output

The inspected HP and HomePortal configurations report the same receiver:
40.4421967375442, -80.05656014497772, 349.65 m. WPGH RF20 (509 MHz) and
WPNT RF21 (515 MHz) are co-sited at 40.495278, -80.004444, 691.8 m. WPXI
RF23 (527 MHz) is at 40.46325, -80.0043056, 615 m. HP's duplicate WPGH
illumination contributes RF evidence but no additional geometric equation.
The setup therefore has two independent transmitter sites. HP bearing was
disabled at inspection; no angular measurement is invented or enabled here.

Two ranges can produce useful geographic **envelopes** and multiple candidate
branches. They cannot generally determine a unique 3-D point. Geographic
usefulness depends strongly on the target direction, distance, range precision,
and the allowed altitude envelope. Motion persistence alone is not proof of
identifiability: a mirror or continuous family can remain physically plausible.

The focused synthetic example uses the exact configured sites, a target at
40.48, -80.12, 5,000 m, 6.144 MS/s lane sampling, and the default 50 m assumed
range-noise sigma. Its envelope has a conservative 14.13 km horizontal radius.
A hypothetical validated 0.5-degree bearing interval reduces that to 2.97 km,
which still fails the default 2 km horizontal qualification gate. These are
synthetic geometry results, **not measured accuracy or operational performance**.
A lower-noise synthetic fixture verifies that the conditional bearing gate can
pass; it does not justify reducing the live noise assumption.

## Solver and uncertainty

`api/altitude-envelope-fusion.js` provides an exact nominal-range construction.
For transmitter local ECEF/ENU vector `t_i`, excess path `e_i`, receiver slant
range `r`, and `s_i = |t_i| + e_i`, each ellipsoid satisfies:

```
t_i · p - s_i r = (|t_i|² - s_i²) / 2
```

Eliminating `r` from two equations gives a plane. Transforming the first
ellipsoid to a sphere gives a circle-plane section; transforming back gives
the complete intersection ellipse. No arbitrary ground plane, guessed height,
fixed vertical velocity, bearing, ADS-B coordinate, or ADS-B association enters.
Coordinates convert back to WGS84 before clipping against the configured
altitude envelope (default 0–20,000 m ellipsoid height) and receiver range
(default 300 km). Terrain is unavailable and is not claimed as a constraint.

The two observable normal directions have a finite local range-Jacobian noise
estimate. The tangent direction remains unresolved and is carried by the whole
curve. Output explicitly has `altitude:null`, `resolved:false`,
`representative_is_fix:false`, `operational_position:false`, and no claimed
confidence level. The envelope includes a three-sigma **assumed local noise
tube**, plus an explicit sampling cover radius. This is neither a calibrated
confidence region nor a proof of complete coverage for arbitrary noisy ranges.
In particular, inconsistent nominal ranges can produce no intersection even
when their measurement-error regions overlap. An empty result is not proof of
no target. Tangencies and singular duplicate geometry do not fabricate a fix.

The two Doppler measurements constrain velocity projections. The solver tests
the minimum possible 3-D speed over an allowed range-rate rectangle, widened
for one Doppler bin and positional uncertainty. It rejects a contour sample
only when that necessary speed exceeds `localization.maximumSpeedMps`
(default 450). No zero climb-rate assumption is introduced.

Same-site near-identical observations are deduplicated without reducing sigma.
Other carriers corroborate only when excess path and wavelength-normalized
Doppler agree. Conflicting returns remain alternative hypotheses. One physical
site always supplies one geometric equation; source false-alarm metadata is
retained and probabilities are never multiplied.

## API and temporal behavior

`fuseTrackCohort()` now returns `position_candidates` separately from the
unchanged three-site `hypotheses` and `tracks` paths. The geographic
representative is an envelope label/center, not a unique point target.

- `position_envelope.vertices` describes the retained 3-D curve segment.
- `horizontal_radius_km` conservatively encloses its horizontal extent,
  normal-noise tube, and sampling coverage about the representative.
- `exact_contour_enu_km` contains center/cosine/sine vectors of the full nominal
  intersection before altitude, range, speed, or optional bearing clipping.
- `geometric_support` is two. `lane_support` and
  `corroborating_contributors` preserve agreeing channel evidence.
- `ENVELOPE_CANDIDATE` becomes `ENVELOPE_SUPPORTED` only after the configured
  number of distinct accepted CPIs have an unambiguous predecessor and pass
  range/Doppler evolution plus a conservative maximum-speed envelope check.
  This is diagnostic persistence, never an operational point or false-track
  qualification. The temporal acceleration allowance is a configured model
  assumption, not a calibrated aircraft dynamics confidence interval.
- Duplicate/replayed/overlapping capture intervals cannot increase support.
  Epoch changes reset it. Stale input yields `ENVELOPE_STALE`; after the
  existing coast timeout, candidates expire.
- Pairing is bounded to 32 observation pairs and 32 candidate branches, with
  at most 8,192 sampled curve points per cohort and 720 per pair. Exceeding
  a cap marks `incomplete-bounded-search` and prevents temporal support.

The existing three-site operational solver and its uncertainty/mirror gates
remain intact. Two-site candidates never set `operational_localization:true`.
UI consumers must render envelopes or regions rather than unique target pins.

## Optional bearing contract

`MultiIlluminatorTrackFuser.update(frames, {bearingConstraints})` and
`fuseTrackCohort(frames, config, limits, {bearingConstraints})` accept an optional
input only when `process.multi_illuminator.localization.bearingConstraintEnabled`
is explicitly true. Nothing enables this setting or supplies data in the live
server. A future cross-node adapter must first establish the following contract:

```
schema: vectorwarp-radar-bearing-constraint-v1
radar_only: true
complete_candidate_set: true
cohort_key, timestamp: exact local target CPI identity
receiver: exactly the configured latitude/longitude/altitude
association:
  method: radar-delay-doppler
  unique: true
  contributor_keys: ["lane:track", "other-lane:track"]
calibration:
  validated: true
  id: nonempty immutable qualification identifier
  artifact_sha256: 64 lowercase hexadecimal characters
  maximum_error_deg: finite positive bound, at most 30
candidates:
  - bearing_deg_true: [0,360)
    half_width_deg: at least calibrated maximum error and at most 90
```

All angular candidates are retained as a union. Missing provenance, stale CPI,
wrong receiver, truth association, incomplete lobes, or unvalidated bounds fail
closed. Duplicate matching constraints are not arbitrarily intersected.
The supplied artifact hash is an identity reference: this module does not load
or independently verify the calibration artifact. The producer/adapter must
perform that verification before asserting `validated:true` or `unique:true`.
The current native `/api/bearing` row has no such capture-association and
validated complete-lobe contract and is deliberately rejected.

`bearing_constraint.horizontally_bounded:true` requires one surviving branch,
verified capture epoch, a complete bounded search, and horizontal extent no
larger than `maximumHorizontalSigmaKm` (default 2 km; for this path it is used
conservatively as a maximum full envelope radius). This is a **conditional
geometric qualifier**. It still leaves `operational_position:false` because
the range noise and false-association behavior have not been field-qualified.
The representative altitude remains null. No operational point marker should
be shown merely because this optional geometric gate passes.

## Validation and remaining acceptance

Focused checks run on one CPU with a 128 MiB Node heap and 30-second timeout:

```
api/altitude-envelope-fusion.test.js
api/multi-illuminator-track-fusion.test.js
api/cross-frequency-support.test.js
```

They cover the analytic equation at sampled points, 18 height/direction cases
with exact live geometry, colocated-carrier/Doppler disagreement, duplicate
illumination uncertainty, impossible Doppler, capture and epoch isolation,
temporal support/staleness/expiry, optional bearing bounds and provenance,
and bit-identical output under changed or removed ADS-B. Existing three-site
localization, uncertainty, mirrored-root and temporal-adversarial tests pass.

Before live qualification: capture immutable channel geometry/configuration
and time alignment; verify measurement noise and whole-pass association on
held-out radar-only inputs; evaluate spatial envelope coverage, width, latency,
fragmentation and false associations/hour with truth isolated to evaluation.
Cover clutter/control windows and missing-carrier behavior. An angular path
also needs a calibrated full-lobe bound and a causally validated cross-node
association adapter. No deployment or model promotion follows from these
synthetic checks.
