'use strict';

// Evaluation/training only. No radar observation is an input to this model.
// NACp bounds: FAA AC 20-165A table 9. NIC/RC is integrity, NOT a Gaussian sigma.
const C = 299792458;
const finite = value => typeof value === 'number' && Number.isFinite(value);
const NAC_P_95_M = [null, 18520, 7408, 3704, 1852, 926, 555.6, 185.2, 92.6, 30, 10, 3];
const dot = (a, b) => a.reduce((sum, x, i) => sum + x * b[i], 0);
const sub = (a, b) => a.map((x, i) => x - b[i]);
const scale = (a, k) => a.map(x => x * k);
const add = (a, b) => a.map((x, i) => x + b[i]);
const radians = degrees => degrees * Math.PI / 180;

function policy(value) {
  // Deliberately no persisted/default enable. Unknown or excessive settings
  // fail closed instead of silently widening the evaluated association family.
  if (value?.enable !== true) return null;
  const result = {maxProjectionS: value.max_projection_s ?? 3,
    accelerationSigma: value.acceleration_sigma_m_s2 ?? 2,
    velocitySigma: value.velocity_sigma_m_s ?? 3,
    verticalPositionSigma: value.vertical_position_sigma_m ?? 45,
    verticalVelocitySigma: value.vertical_velocity_sigma_m_s ?? 3,
    maxAreaBins: value.max_area_bins ?? 128};
  const ceilings = {maxProjectionS: 5, accelerationSigma: 10, velocitySigma: 20,
    verticalPositionSigma: 300, verticalVelocitySigma: 20, maxAreaBins: 256};
  if (Object.entries(result).some(([key, number]) => !finite(number) ||
      number <= 0 || number > ceilings[key])) throw new Error('Invalid evaluation-only truth uncertainty policy');
  return result;
}

// Catch-up holds permit later observations, never older truth. Acquisition
// also caps future projection at elapsed CPI-to-fetch time; uncertainty wins.
function scoringProjectionPolicy(config) {
  const adsb = config.truth?.adsb || {}, uncertainty = policy(adsb.uncertainty_association);
  const maxAge = finite(adsb.max_position_age) ? adsb.max_position_age : 30;
  const held = Number.isInteger(adsb.association_lag_cpis) && adsb.association_lag_cpis > 0;
  return {schema: 'vectorwarp-adsb-scoring-projection-v1',
    past_s: Math.min(maxAge, uncertainty?.maxProjectionS ?? 1),
    future_s: Math.min(maxAge, uncertainty?.maxProjectionS ?? (held ? maxAge : 1)),
    future_bound: held && !uncertainty ? 'elapsed-target-to-fetch' : 'fixed'};
}

function enu(latitude, longitude) {
  const p = radians(latitude), l = radians(longitude);
  return [[-Math.sin(l), Math.cos(l), 0],
    [-Math.sin(p) * Math.cos(l), -Math.sin(p) * Math.sin(l), Math.cos(p)],
    [Math.cos(p) * Math.cos(l), Math.cos(p) * Math.sin(l), Math.sin(p)]];
}

function velocity(aircraft) {
  if (!finite(aircraft?.gs) || aircraft.gs < 0 || aircraft.gs > 1000 ||
      !finite(aircraft.track) || !finite(aircraft.lat) || !finite(aircraft.lon)) return null;
  const axes = enu(aircraft.lat, aircraft.lon), heading = radians(aircraft.track);
  const up = finite(aircraft.geom_rate) ? aircraft.geom_rate : aircraft.baro_rate;
  const speed = aircraft.gs * .514444;
  return {ecef: add(add(scale(axes[0], speed * Math.sin(heading)),
    scale(axes[1], speed * Math.cos(heading))), scale(axes[2], finite(up) ? up * .3048 / 60 : 0)),
  axes, speed, verticalKnown: finite(up)};
}

function geometry(target, rx, tx, v, frequency) {
  if (![...target, ...rx, ...tx, ...v, frequency].every(finite) || frequency <= 0) return null;
  const rr = sub(target, rx), rt = sub(target, tx);
  const dr = Math.hypot(...rr), dt = Math.hypot(...rt);
  if (dr < 1 || dt < 1) return null;
  const ur = scale(rr, 1 / dr), ut = scale(rt, 1 / dt), q = add(ur, ut);
  const h = add(scale(sub(v, scale(ur, dot(ur, v))), 1 / dr),
    scale(sub(v, scale(ut, dot(ut, v))), 1 / dt));
  const wavelength = C / frequency;
  return {doppler: -dot(q, v) / wavelength,
    delayRate: dot(q, v) / 1000,
    geometricDopplerRate: -dot(v, h) / wavelength,
    positionRows: [scale(q, .001), scale(h, -1 / wavelength)],
    velocityRows: [[0, 0, 0], scale(q, -1 / wavelength)]};
}

function projectedUncertainty({aircraft, target, rx, tx, frequency, config, motion = {}}) {
  if (!config) return null;
  const provenance = aircraft.vectorwarp || {}, observation = provenance.observation;
  const t = provenance.observation_time_offset_s;
  const invalid = reason => ({schema: 'vectorwarp-truth-uncertainty-v1', valid: false,
    reason, evaluation_only: true, null_calibrated: false});
  if (!observation || observation.schema !== 'vectorwarp-adsb-observation-v1')
    return invalid('raw-observation-lineage-unavailable');
  if (!finite(t) || Math.abs(t) > config.maxProjectionS) return invalid('projection-horizon');
  const source = provenance.position_source;
  // MLAT/TISB accuracy cannot be inferred from the transponder's GNSS NACp.
  if (!['ADSB_DIRECT', 'ADSR'].includes(source)) return invalid('source-accuracy-unbounded');
  const nac = observation.nac_p;
  const radius = Number.isInteger(nac) && nac >= 1 && nac <= 11 ? NAC_P_95_M[nac] : null;
  if (!radius || !(Number.isInteger(observation.nic) && observation.nic > 0 && observation.nic <= 11))
    return invalid('position-accuracy-or-integrity-unavailable');
  const reportedDisagreement = provenance.maximum_pairwise_projected_disagreement_km;
  if (reportedDisagreement !== undefined && reportedDisagreement !== null &&
      (!finite(reportedDisagreement) || reportedDisagreement < 0))
    return invalid('invalid-source-disagreement');
  const disagreementM = finite(reportedDisagreement) ? reportedDisagreement * 1000 : 0;
  const v = velocity(observation), projectedV = velocity(aircraft);
  if (!v || !projectedV) return invalid('velocity-unavailable');
  const g = geometry(target, rx, tx, projectedV.ecef, frequency);
  if (!g) return invalid('invalid-geometry');
  // Fusion already bounds its accepted source cluster. Its observed separation
  // can exceed the winning transponder's NACp: retain the full separation as a
  // scale floor, then let the association area's hard cap abstain if necessary.
  // Taking max avoids inventing independence between provider errors.
  const sigmaHorizontal = Math.max(radius / Math.sqrt(5.991464547107979), disagreementM);
  const turn = Math.max(finite(observation.track_rate) ? Math.abs(observation.track_rate) : 0,
    finite(motion.turnRateDegS) ? Math.abs(motion.turnRateDegS) : 0);
  const observedAcceleration = finite(motion.accelerationMS2) ? Math.abs(motion.accelerationMS2) : 0;
  const verticalAcceleration = finite(motion.verticalAccelerationMS2) ? Math.abs(motion.verticalAccelerationMS2) : 0;
  // Motion spread is a declared conservative model, not calibrated probability.
  // Turns are not extrapolated as a known control: their acceleration increases
  // uncertainty; a sufficiently uncertain comparison abstains at the hard cap.
  const acceleration = Math.max(config.accelerationSigma, observedAcceleration, verticalAcceleration,
    v.speed * radians(turn));
  const covariance = [0, 0, 0, 0];
  const contribute = (a, b, variance) => {
    covariance[0] += a * a * variance; covariance[1] += a * b * variance;
    covariance[2] += a * b * variance; covariance[3] += b * b * variance;
  };
  for (let axis = 0; axis < 3; ++axis) {
    const basis = v.axes[axis];
    const p = g.positionRows.map(row => dot(row, basis));
    const w = g.velocityRows.map(row => dot(row, basis));
    const positionSigma = axis < 2 ? sigmaHorizontal :
      config.verticalPositionSigma * (finite(observation.alt_geom) ? 1 : 3);
    const velocitySigma = axis < 2 ? config.velocitySigma :
      config.verticalVelocitySigma * (v.verticalKnown ? 1 : 3);
    contribute(p[0], p[1], positionSigma ** 2);
    contribute(p[0] * t + w[0], p[1] * t + w[1], velocitySigma ** 2);
    contribute(.5 * t * t * p[0] + t * w[0],
      .5 * t * t * p[1] + t * w[1], acceleration ** 2);
  }
  if (!covariance.every(finite)) return invalid('nonfinite-covariance');
  return {schema: 'vectorwarp-truth-uncertainty-v1', valid: true, evaluation_only: true,
    null_calibrated: false, model: 'linearized-bistatic-conditional-motion-v1',
    covariance, doppler_hz: g.doppler, delay_rate_km_s: g.delayRate,
    geometric_doppler_rate_hz_s: g.geometricDopplerRate,
    // Timing displacement is correlated in delay and Doppler. Keep a separate
    // maneuver bound because missing turn reports must not imply zero motion.
    doppler_rate_bound_hz_s: Math.abs(g.geometricDopplerRate) + 2 * frequency / C * acceleration,
    observation_offset_s: t, nac_p: nac, nic: observation.nic,
    integrity_containment_radius_m: finite(observation.rc) && observation.rc > 0 ? observation.rc : null,
    source_class: source, position_radius_95_m: radius,
    source_disagreement_m: finite(reportedDisagreement) ? disagreementM : null,
    horizontal_position_sigma_m: sigmaHorizontal,
    acceleration_sigma_m_s2: acceleration, turn_rate_deg_s: turn,
    velocity_sigma_m_s: config.velocitySigma,
    vertical_position_sigma_m: config.verticalPositionSigma,
    snapshot_age_at_receipt_s: provenance.snapshot_age_at_receipt_s ?? null,
    assumptions: 'NACp isotropic Gaussian conversion; declared velocity/altitude/acceleration spreads; no NIC-to-sigma conversion'};
}

module.exports = {policy, scoringProjectionPolicy, velocity, geometry, projectedUncertainty};
