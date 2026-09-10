'use strict';

// Independent release acceptance cases. These exposed failures in the
// initial pure audit; passing them does not mean array bearing is implemented.
// Run in the existing capped source container, never start acquisition for them.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {evaluateKrakenArray, localAzimuthToTrueBearing, spacingAssessment} =
  require('./kraken-array');
const {compareKrakenStatus} = require('./upstream-status');

const C = 299792458;
const cases = [];
const check = (name, run) => cases.push({name, run});
const clone = value => JSON.parse(JSON.stringify(value));
const config = () => ({capture: {fc: 100000000, fs: 2400000, device: {
  type: 'Kraken', channel_count: 5, reference_channel: 2,
  surveillance_channels: [4, 0, 3, 1],
  array_geometry: {
    shape: 'cross', units: 'm', coordinate_frame: 'array_xy_right_handed',
    mapping_confirmed: true, geometry_confirmed: true,
    orientation_confirmed: true, x_axis_bearing_deg_true: 90,
    bearing_channels: [4, 0, 3, 1],
    elements: [
      {id: 'plus-x', daq_channel: 0, receiver_serial: 'review-A', position: [.5, 0, 0]},
      {id: 'reference', daq_channel: 2, receiver_serial: 'review-R', position: [2, 2, .2]},
      {id: 'minus-x', daq_channel: 3, receiver_serial: 'review-B', position: [-.5, 0, 0]},
      {id: 'minus-y', daq_channel: 1, receiver_serial: 'review-C', position: [0, -.5, 0]},
      {id: 'plus-y', daq_channel: 4, receiver_serial: 'review-D', position: [0, .5, 0]}
    ]
  }
}}, process: {reference_synthesis: {mode: 'dedicated', channels: [2]}}});

const rawStatus = () => ({num_channels: 5, max_elements: 8,
  settings: {center_freq: 100000000, sample_rate: 2400000, gain: 0},
  operating_mode: 'coherent', reconfiguring: false, recovering: false,
  cooldown_active: false});
const observed = (cfg, changes = {}) => ({available: true, checkedAt: Date.now(),
  ...compareKrakenStatus(cfg, {...rawStatus(), ...changes})});
const ineligible = result => {
  assert.equal(result.relativeBearingEligible, false);
  assert.equal(result.worldBearingEligible, false);
};

check('legacy missing geometry remains passive-radar-valid and bearing-ineligible', () => {
  const cfg = config(); delete cfg.capture.device.array_geometry;
  const result = evaluateKrakenArray(cfg, observed(cfg));
  assert.equal(result.valid, true); ineligible(result);
});
check('cross maps an arbitrary dedicated reference outside both bearing and surveillance', () => {
  const cfg = config(); const result = evaluateKrakenArray(cfg, observed(cfg));
  assert.equal(result.valid, true);
  assert.deepEqual(result.mapping.filter(row => row.bearing).map(row => row.daqChannel).sort(), [0, 1, 3, 4]);
  const ref = result.mapping.find(row => row.daqChannel === 2);
  assert.equal(ref.bearing, false); assert.equal(ref.surveillance, false);
  assert.equal(result.trackerAllowed, false);
});
check('count-only status cannot establish live bearing eligibility', () => {
  const result = evaluateKrakenArray(config(), {num_channels: 5});
  ineligible(result);
  assert.equal(result.verification.physicalAgreement.state, 'unverified');
  assert.equal(result.verification.channelCount.state, 'reported-match');
  assert.equal(result.verification.statusFreshness.state, 'unverified');
});
check('missing live status cannot establish live bearing eligibility', () => {
  ineligible(evaluateKrakenArray(config()));
});
check('explicitly unavailable status cannot reuse a previously matching count', () => {
  const cfg = config();
  ineligible(evaluateKrakenArray(cfg, {...observed(cfg), available: false}));
});
check('stale status cannot reuse a previously matching count', () => {
  const cfg = config();
  ineligible(evaluateKrakenArray(cfg, {...observed(cfg), checkedAt: 1}));
});
for (const [name, patch] of [
  ['reconfiguring', {reconfiguring: true}],
  ['recovering', {recovering: true}],
  ['calibrating', {cooldown_active: true}],
  ['wideband', {operating_mode: 'wideband'}],
  ['wrong RF', {settings: {center_freq: 200000000, sample_rate: 2400000}}],
  ['wrong sample rate', {settings: {center_freq: 100000000, sample_rate: 1200000}}]
]) check(`${name} upstream state cannot establish live bearing eligibility`, () => {
  const cfg = config(); ineligible(evaluateKrakenArray(cfg, observed(cfg, patch)));
});
check('conflicting raw and normalized count evidence does not select the convenient match', () => {
  const result = evaluateKrakenArray(config(), {num_channels: 5, actual: {channels: 4}});
  ineligible(result);
  assert.equal(result.verification.channelCount.state, 'conflict');
  assert.ok(result.issues.some(issue => issue.code === 'live-channel-count-conflict'));
});
check('changing the active count invalidates the retained five-channel geometry', () => {
  const cfg = config(); cfg.capture.device.channel_count = 4;
  const result = evaluateKrakenArray(cfg, {num_channels: 4});
  assert.equal(result.valid, false); ineligible(result);
});
check('invalid huge count fails validation without throwing or allocating an unbounded array', () => {
  const cfg = config(); cfg.capture.device.channel_count = 2 ** 32;
  const result = evaluateKrakenArray(cfg, {num_channels: 5});
  assert.equal(result.valid, false); ineligible(result);
});
check('unequal collinear spacing is custom geometry, not a uniform linear array', () => {
  const cfg = config(); const geometry = cfg.capture.device.array_geometry;
  geometry.shape = 'ula';
  geometry.elements.filter(row => row.daq_channel !== 2)
    .forEach((row, i) => { row.position = [[0, 0, 0], [.2, 0, 0], [.6, 0, 0], [1.4, 0, 0]][i]; });
  const result = evaluateKrakenArray(cfg, observed(cfg));
  assert.equal(result.valid, false);
});
check('a vertical-only aperture cannot qualify for azimuth bearing', () => {
  const cfg = config(); const geometry = cfg.capture.device.array_geometry;
  geometry.shape = 'custom';
  geometry.elements.filter(row => row.daq_channel !== 2)
    .forEach((row, i) => { row.position = [0, 0, i * .2]; });
  const result = evaluateKrakenArray(cfg, observed(cfg));
  ineligible(result);
  assert.ok(result.issues.some(issue => issue.code === 'azimuth-unobservable'));
});
check('calling a collinear array custom does not remove front/back ambiguity', () => {
  const cfg = config(); const geometry = cfg.capture.device.array_geometry;
  geometry.shape = 'custom';
  geometry.elements.filter(row => row.daq_channel !== 2)
    .forEach((row, i) => { row.position = [i * .2, 0, 0]; });
  const result = evaluateKrakenArray(cfg, observed(cfg));
  assert.ok(result.issues.some(issue => /ambigu|collinear|rank/i.test(`${issue.code} ${issue.message}`)));
});
check('a nearest-neighbor heuristic cannot declare an exactly aliased rectangular array safe', () => {
  const frequency = 100000000; const wavelength = C / frequency;
  const points = [[0, 0, 0], [wavelength, 0, 0], [0, .1 * wavelength, 0], [wavelength, .1 * wavelength, 0]];
  // u1=(.5,sqrt(.75),0), u2=(-.5,sqrt(.75),0): unit directions.
  // Their per-element phase difference is 2*pi*x/lambda = 0 or 2*pi.
  for (const point of points) {
    const phaseDifference = 2 * Math.PI * point[0] / wavelength;
    assert.ok(Math.abs(Math.cos(phaseDifference) - 1) < 1e-12);
    assert.ok(Math.abs(Math.sin(phaseDifference)) < 1e-12);
  }
  const result = spacingAssessment(points, 'custom', frequency);
  assert.notEqual(result.aliasRisk, false,
    'Return possible/unknown aliasing for arbitrary manifolds; small nearest-neighbor gaps do not prove absence.');
});
check('right-handed cardinal conversion is consistent for +x=east and +y=north', () => {
  assert.deepEqual([0, 90, 180, 270].map(angle => localAzimuthToTrueBearing(angle, 90)), [90, 0, 270, 180]);
  assert.deepEqual([0, 90, 180, 270].map(angle => localAzimuthToTrueBearing(angle, 0)), [0, 270, 180, 90]);
});
check('a fully populated healthy status and asserted survey still cannot enable unimplemented bearing', () => {
  const cfg = config(); const result = evaluateKrakenArray(cfg, observed(cfg));
  assert.equal(result.valid, true);
  assert.equal(result.bearingImplemented, false);
  assert.equal(result.verification.physicalAgreement.state, 'unverified');
  assert.equal(result.verification.physicalGeometry.state, 'operator-asserted');
  ineligible(result);
});
check('count disagreement is observation evidence, not a structural geometry error', () => {
  const cfg = config(); const result = evaluateKrakenArray(cfg, {num_channels: 4});
  assert.equal(result.valid, true);
  assert.equal(result.verification.channelCount.state, 'reported-mismatch');
  ineligible(result);
});
check('array size caps are checked before reading array entries', () => {
  for (const field of ['elements', 'bearing_channels']) {
    const cfg = config(); const values = new Array(9);
    Object.defineProperty(values, 0, {get() { throw new Error('Oversized input was traversed'); }});
    cfg.capture.device.array_geometry[field] = values;
    const result = evaluateKrakenArray(cfg, observed(cfg));
    assert.equal(result.valid, false); ineligible(result);
  }
});
check('sparse coordinates, channel lists, and element lists are rejected', () => {
  for (const mutate of [
    cfg => { cfg.capture.device.array_geometry.elements[0].position = new Array(3); },
    cfg => { cfg.capture.device.array_geometry.bearing_channels = new Array(4); },
    cfg => { cfg.capture.device.array_geometry.elements = new Array(5); },
    cfg => { cfg.capture.device.surveillance_channels = new Array(4); }
  ]) {
    const cfg = config(); mutate(cfg);
    assert.equal(evaluateKrakenArray(cfg, observed(cfg)).valid, false);
  }
});
check('finite coordinates outside the safe arithmetic range cannot produce accepted geometry', () => {
  const cfg = config();
  cfg.capture.device.array_geometry.elements[0].position = [Number.MAX_VALUE, 0, 0];
  const result = evaluateKrakenArray(cfg, observed(cfg));
  assert.equal(result.valid, false);
  assert.equal(result.mapping.find(row => row.daqChannel === 0).positionM, null);
});
check('equally spaced rotated ULAs remain structurally valid in arbitrary DAQ order', () => {
  const cfg = config(); const geometry = cfg.capture.device.array_geometry;
  geometry.shape = 'ula';
  geometry.elements.filter(row => row.daq_channel !== 2)
    .forEach((row, i) => { const t = [3, 0, 2, 1][i]; row.position = [.2 * t, .3 * t, .1 * t]; });
  const result = evaluateKrakenArray(cfg, observed(cfg));
  assert.equal(result.valid, true);
  assert.ok(result.issues.some(issue => issue.code === 'ula-front-back-ambiguity'));
  ineligible(result);
});
check('a vertical cross is custom geometry rather than the horizontal cross shorthand', () => {
  const cfg = config();
  cfg.capture.device.array_geometry.elements.filter(row => row.daq_channel !== 2)
    .forEach(row => { row.position = [row.position[0], 0, row.position[1]]; });
  const result = evaluateKrakenArray(cfg, observed(cfg));
  assert.equal(result.valid, false);
  assert.ok(result.issues.some(issue => issue.code === 'cross-geometry'));
});
check('spacing assessment rejects malformed and oversized standalone inputs', () => {
  for (const points of [null, {}, new Array(3), new Array(9), [[0, 0], [1, 0, 0]], [[0, 0, 0], [Infinity, 0, 0]]]) {
    const result = spacingAssessment(points, 'custom', 100000000);
    assert.equal(result.assessed, false);
    assert.equal(result.aliasRisk, null);
  }
});
check('a ULA at exactly half wavelength preserves endpoint alias risk', () => {
  const spacing = C / 100000000 / 2;
  const result = spacingAssessment([[0, 0, 0], [spacing, 0, 0]], 'ula', 100000000);
  assert.equal(result.aliasRisk, true);
});
check('unknown reference synthesis mode is rejected rather than bypassing dedicated constraints', () => {
  const cfg = config(); cfg.process.reference_synthesis.mode = 'dedciated';
  assert.equal(evaluateKrakenArray(cfg, observed(cfg)).valid, false);
});
check('ADS-B changes cannot change geometry or bearing acceptance', () => {
  const cfg = config(); const status = observed(cfg);
  const truthChanged = clone(cfg);
  truthChanged.truth = {adsb: {enabled: true, aircraft: [{bearing: 271, lat: 40, lon: -74}]}};
  assert.deepEqual(evaluateKrakenArray(cfg, status), evaluateKrakenArray(truthChanged, status));
});
check('current detector and tracker schemas have no bearing or ADS-B input', () => {
  for (const relative of ['src/data/Detection.h', 'src/process/tracker/Tracker.h']) {
    const text = fs.readFileSync(path.join(__dirname, '..', relative), 'utf8');
    assert.doesNotMatch(text, /\b(?:bearing|azimuth|adsb|array_geometry)\b/i, relative);
  }
  const main = fs.readFileSync(path.join(__dirname, '../src/blah2.cpp'), 'utf8');
  assert.match(main, /tracker->process\(detection\.get\(\), trackerTime\)/);
});

let failures = 0;
for (const {name, run} of cases) {
  try { run(); console.log(`PASS ${name}`); }
  catch (error) { failures++; console.error(`FAIL ${name}: ${error.message.split('\n')[0]}`); }
}
console.log(`${cases.length - failures}/${cases.length} independent Kraken acceptance cases passed; ${failures} release acceptance failures.`);
process.exitCode = failures ? 1 : 0;
