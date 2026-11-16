'use strict';

const assert = require('assert');
const {policy, velocity, geometry, projectedUncertainty} = require('./truth-uncertainty');
const {ecef, distance, DelayDopplerHistory} = require('./adsb-geometry');
const {fuseAircraftData, projectAircraft, separationKm} = require('./adsb-fusion');
const {associationGates, assignments} = require('../html/js/config_profiles');
const {limits, siteKey} = require('./evidence-statistics');
const configuration = {capture: {fc: 509e6, fs: 2.4e6},
  truth: {adsb: {uncertainty_association: {enable: true}, smoothing_window: 10}},
  process: {data: {cpi: .5}, ambiguity: {delayMin: -10, delayMax: 500,
    dopplerMin: -600, dopplerMax: 600}, detection: {minDelay: 5}},
  location: {rx: {latitude: 0, longitude: 0, altitude: 0},
    tx: {latitude: 0, longitude: 1, altitude: 0}}};
const settings = policy(configuration.truth.adsb.uncertainty_association);
assert.equal(policy(undefined), null);
assert.equal(policy({enable: false}), null);
assert.throws(() => policy({enable: true, max_projection_s: 100}));
assert.throws(() => policy({enable: true, acceleration_sigma_m_s2: NaN}));
const original = {hex: 'abc123', type: 'adsb_icao', lat: .2, lon: .5,
  alt_geom: 10000, gs: 450, track: 90, geom_rate: 0, nic: 9, nac_p: 9, seen_pos: 0};
const targetTime = 1000;
const raw = (aircraft = original, at = targetTime - .1) =>
  ({source: {id: 'readsb', kind: 'file'}, receivedAtMs: targetTime * 1000,
    data: {now: at, aircraft: [{...aircraft, seen_pos: 0}]}});
const fused = fuseAircraftData([raw()], {nowMs: targetTime * 1000});
const aircraft = fused.aircraft[0];
const rx = ecef(0, 0, 0), tx = ecef(0, 1, 0);
const target = ecef(aircraft.lat, aircraft.lon, aircraft.alt_geom * .3048);
const model = projectedUncertainty({aircraft, target, rx, tx,
  frequency: 509e6, config: settings});
assert.equal(model.valid, true);
assert.equal(model.position_radius_95_m, 30);
assert.equal(model.nic, 9);
assert.equal(model.null_calibrated, false);
assert.ok(model.covariance.every(Number.isFinite));
const withDisagreement = disagreement => projectedUncertainty({aircraft: {...aircraft,
  vectorwarp: {...aircraft.vectorwarp, maximum_pairwise_projected_disagreement_km: disagreement}},
target, rx, tx, frequency: 509e6, config: settings});
assert.equal(withDisagreement(.45).reason, 'source-disagreement-exceeds-accuracy',
  '450 m accepted-cluster separation cannot retain the selected 30 m NACp accuracy');
assert.equal(withDisagreement(.03 + 1e-9).valid, false, 'above-accuracy disagreement abstains');
const boundaryDisagreement = withDisagreement(.03);
assert.equal(boundaryDisagreement.valid, true, 'exact declared-accuracy boundary is admitted');
assert.equal(boundaryDisagreement.horizontal_position_sigma_m, 30);
assert.ok(boundaryDisagreement.covariance[0] > model.covariance[0],
  'admitted source disagreement propagates into delay covariance');
for (const disagreement of [0, null, undefined])
  assert.deepEqual(withDisagreement(disagreement).covariance, model.covariance,
    'absent/single-source or zero disagreement preserves the declared source model');
assert.equal(withDisagreement(-.1).reason, 'invalid-source-disagreement');

// Independent finite-difference check of BOTH position Jacobian rows. This
// catches the otherwise easily omitted v^T H v geometric Doppler-rate term.
const v = velocity(aircraft).ecef;
const g = geometry(target, rx, tx, v, 509e6);
for (let axis = 0; axis < 3; ++axis) {
  const plus = [...target], minus = [...target]; plus[axis] += .1; minus[axis] -= .1;
  const delayDerivative = ((distance(plus, rx) + distance(plus, tx)) -
    (distance(minus, rx) + distance(minus, tx))) / .2 / 1000;
  const dopplerDerivative = (geometry(plus, rx, tx, v, 509e6).doppler -
    geometry(minus, rx, tx, v, 509e6).doppler) / .2;
  assert.ok(Math.abs(delayDerivative - g.positionRows[0][axis]) < 1e-8);
  assert.ok(Math.abs(dopplerDerivative - g.positionRows[1][axis]) < 1e-7);
}
const movingPlus = target.map((x, i) => x + v[i] * .001);
const movingMinus = target.map((x, i) => x - v[i] * .001);
assert.ok(Math.abs((geometry(movingPlus, rx, tx, v, 509e6).doppler -
  geometry(movingMinus, rx, tx, v, 509e6).doppler) / .002 - g.geometricDopplerRate) < 1e-5);

const history = new DelayDopplerHistory(configuration);
const prediction = history.update(fused).data.abc123;
assert.ok(prediction, 'opt-in instantaneous velocity needs no lagged position-difference warmup');
assert.ok(Math.abs(prediction.doppler - model.doppler_hz) < 1e-9);
const disabled = JSON.parse(JSON.stringify(configuration));
delete disabled.truth.adsb.uncertainty_association;
assert.deepEqual(new DelayDopplerHistory(disabled).update(fused).data, {},
  'default history/warmup behavior is unchanged');
assert.notEqual(siteKey(disabled), siteKey(configuration), 'evaluation policy has a separate scorecard epoch');

// Signed ages and arbitrary relay chains must project the original observation
// once, including an observation newer than a historical target frame.
const future = raw({...original, track: 35}, targetTime + .5);
const back = fuseAircraftData([future], {nowMs: targetTime * 1000});
let relay = back;
for (const at of [targetTime - .25, targetTime + .2, targetTime - .4]) {
  relay = fuseAircraftData([{source: {id: 'relay', kind: 'http'}, data: relay,
    receivedAtMs: (at + .018) * 1000}], {nowMs: at * 1000});
  assert.equal(relay.aircraft.length, 1);
  const item = relay.aircraft[0];
  const expected = projectAircraft(future.data.aircraft[0], at - future.data.now).aircraft;
  assert.ok(separationKm(item, expected) < 1e-10);
  assert.equal(item.vectorwarp.observation_position_timestamp_s, future.data.now);
  assert.equal(item.vectorwarp.observation.timestamp_s, future.data.now);
  assert.equal(item.vectorwarp.projection_lineage, 'single-original-observation');
}
const agedRelay = fuseAircraftData([raw(original, targetTime - 2.9)],
  {nowMs: targetTime * 1000, maxObservationOffsetSeconds: 3});
const compete = fuseAircraftData([{source: {id: 'relay'}, data: agedRelay},
  raw({...original, lon: .501}, targetTime - .27)], {nowMs: targetTime * 1000});
assert.equal(compete.aircraft[0].vectorwarp.source_id, 'readsb',
  'relay publication time cannot defeat a newer actual observation');
assert.ok(Math.abs(agedRelay.aircraft[0].vectorwarp.observation_time_offset_s - 2.9) < 1e-9);
const agedModel = projectedUncertainty({aircraft: agedRelay.aircraft[0], target, rx, tx,
  frequency: 509e6, config: settings});
assert.ok(agedModel.covariance[3] > model.covariance[3]);
for (const className of ['MLAT', 'TISB', 'OTHER']) {
  assert.equal(projectedUncertainty({aircraft: {...aircraft,
    vectorwarp: {...aircraft.vectorwarp, position_source: className}}, target, rx, tx,
  frequency: 509e6, config: settings}).reason, 'source-accuracy-unbounded');
}
for (const missing of ['nic', 'nac_p']) {
  const observation = {...aircraft.vectorwarp.observation}; delete observation[missing];
  assert.equal(projectedUncertainty({aircraft: {...aircraft,
    vectorwarp: {...aircraft.vectorwarp, observation}}, target, rx, tx,
  frequency: 509e6, config: settings}).valid, false);
}
const turning = projectedUncertainty({aircraft, target, rx, tx, frequency: 509e6,
  config: settings, motion: {turnRateDegS: 3, accelerationMS2: 4}});
assert.ok(turning.acceleration_sigma_m_s2 > 12, '450 kt at 3 deg/s exceeds 12 m/s^2');
assert.ok(turning.covariance[3] > model.covariance[3]);
assert.equal(projectedUncertainty({aircraft: {...aircraft, vectorwarp: {...aircraft.vectorwarp,
  observation: {...aircraft.vectorwarp.observation, track_rate: 0}}}, target, rx, tx,
frequency: 509e6, config: settings, motion: {turnRateDegS: 3}}).turn_rate_deg_s, 3,
'a stale zero turn-rate field cannot hide measured course changes');
const changedSource = raw({...original, gs: 500}, targetTime + .9);
changedSource.source = {id: 'independent-source', kind: 'file'};
const switched = history.update(fuseAircraftData([changedSource],
  {nowMs: (targetTime + 1) * 1000})).data.abc123;
assert.equal(switched.truth_uncertainty.acceleration_sigma_m_s2, settings.accelerationSigma,
  'velocity disagreement across providers is not an observed aircraft acceleration');
const climbing = {...aircraft, geom_rate: 1800, vectorwarp: {...aircraft.vectorwarp,
  observation: {...aircraft.vectorwarp.observation, geom_rate: 1800}}};
assert.notEqual(projectedUncertainty({aircraft: climbing, target, rx, tx,
  frequency: 509e6, config: settings}).doppler_hz, model.doppler_hz);

const span = limits(configuration);
const alignment = {declared: true, model: 'existing-operational-provisional', frameOffsetS: 0};
const gate = associationGates(span, prediction, targetTime * 1000, 'detection', 0, alignment);
assert.equal(gate.valid, true);
assert.equal(gate.shape, 'ellipse');
assert.equal(associationGates({...span, truthUncertaintyPolicy: null}, prediction,
  targetTime * 1000).shape, undefined, 'legacy gate family remains the default');
const inflated = {...prediction, truth_uncertainty: {...model, covariance: [1, 0, 0, 1e6]}};
assert.equal(associationGates(span, inflated, targetTime * 1000, 'detection', 0,
  alignment).reason, 'uncertainty-exceeds-hard-cap', 'never silently clip a broad ellipse');
const diagonal = {...gate, covariance: [.01, 0, 0, 1], determinant: .01,
  maximumNis: 1, delayKm: .1, dopplerHz: 1};
const predictions = [['abc123', {delay: 10, doppler: 100}]];
const point = (delay, doppler, index = 0) => ({index, rawDelay: delay, delay, doppler});
assert.equal(assignments([point(10.09, 100.9)], predictions, () => diagonal,
  'detection', 1).size, 0, 'rectangle corner is outside the ellipse');
assert.equal(assignments([point(10.05, 100)], predictions, () => diagonal,
  'detection', 1).size, 1);
assert.equal(assignments([point(10, 100)], [...predictions,
  ['def456', {delay: 10, doppler: 100}]], () => diagonal, 'detection', 1).size, 0,
  'one radar point cannot identify two coincident aircraft');
assert.equal(assignments([point(9.99, 100), point(10.01, 100, 1)], predictions,
  () => diagonal, 'detection', 1).size, 0, 'two equivalent peaks must abstain, not select arbitrary truth');
assert.equal(assignments([point(10, 100), point(10.08, 100, 1)], predictions,
  () => diagonal, 'detection', 1).size, 1, 'one aircraft has at most one selected radar point');
assert.equal(assignments([point(20, 150)], predictions, () => diagonal,
  'detection', 1).size, 0, 'translated null control cannot gain an association');
console.log('PASS opt-in ADS-B raw lineage, analytic motion, uncertainty ellipse and negative controls');
