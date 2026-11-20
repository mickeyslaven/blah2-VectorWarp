'use strict';
const assert = require('node:assert/strict');
const {inverse, locate, BistaticHypothesisTracker, locationsFromBearing} = require('./bistatic-geolocation');
const rx = {latitude: 40, longitude: -80};
const tx = {latitude: 40.05, longitude: -80};
const baseline = inverse(rx, tx);
assert.ok(baseline.distanceM > 5000 && baseline.distanceM < 6000);
const along = locate(rx, tx, 1000, baseline.bearingDeg);
assert.ok(along && along.rangeM > baseline.distanceM);
const opposite = locate(rx, tx, 1000, baseline.bearingDeg + 180);
assert.ok(opposite && opposite.rangeM < along.rangeM);
const config = {capture: {fs: 2400000}, process: {bearing: {enable: true}}, location: {rx, tx}};
const result = locationsFromBearing({timestamp: 1, data: [{id: 'A001', delay: 80,
  bearing_deg_true: 90, uncertainty_deg: 12, resolved: true,
  candidates: [{bearing_deg_true: 90, support: .8}]}]}, config);
assert.equal(result.data.length, 1);
assert.equal(result.data[0].location, null);
assert.equal(result.operational_localization, false);
assert.equal(result.data[0].operational_position, false);
assert.equal(result.data[0].altitude_resolved, false);
assert.equal(result.data[0].covariance_3d, null);
assert.ok(result.data[0].estimate && result.data[0].diagnostic_surface_corridor.length === 3);
assert.deepEqual(result.data[0].uncertainty_corridor, []);
const unresolved = locationsFromBearing({timestamp: 2, data: [{id: 'A002', delay: 80,
  state: 'COASTING', bearing_deg_true: 135, uncertainty_deg: 40, resolved: false,
  candidates: [{bearing_deg_true: 90, support: .55}]}]}, config);
assert.equal(unresolved.data[0].location, null);
assert.ok(unresolved.data[0].estimate);
assert.equal(unresolved.data[0].uncertainty_corridor.length, 0);
const replay = {...config, capture: {fs: 2400000, replay: {state: true,
  sample_rate_hz: 2000000}}};
const nativeFrame = {timestamp: 1, sample_rate_hz: 6144000, data: [{id: 'replay', delay: 80,
  bearing_deg_true: 90, uncertainty_deg: 12, resolved: true}]};
assert.equal(locationsFromBearing(nativeFrame, replay).available, false, 'Replay cannot inherit live sites');
require('../test/helpers/trusted-replay-sites')(replay);
const nativeResult = locationsFromBearing(nativeFrame, replay);
assert.equal(nativeResult.sample_rate_hz, 6144000);
assert.equal(nativeResult.sample_rate_source, 'native-product');
assert.equal(nativeResult.data[0].excess_path_km, 80 * 299792458 / 6144000 / 1000);
for (const uncertainty of [-1, NaN, Infinity, undefined])
  assert.deepEqual(locationsFromBearing({...nativeFrame, data: [
    {...nativeFrame.data[0], uncertainty_deg: uncertainty}]}, replay).data, []);
for (const timestamp of [null, NaN, 0])
  assert.equal(locationsFromBearing({...nativeFrame, timestamp}, replay).available, false);
// Old or replayed surface estimates stay nonoperational, even if an upstream
// bearing labels itself resolved or claims a geographic-fusion method.
for (const timestamp of [1, Date.now() - 3600000]) {
  const old = locationsFromBearing({...nativeFrame, timestamp, method: 'qualified-multisite'}, replay);
  assert.equal(old.operational_localization, false);
  assert.equal(old.data[0].location, null);
  assert.equal(old.data[0].operational_position, false);
}
assert.equal(locationsFromBearing({...nativeFrame, sample_rate_hz: 0}, replay).available, false);
assert.equal(locationsFromBearing({data: []}, {...config, process: {bearing: {enable: false}}}).enabled, false);
const hypotheses = new BistaticHypothesisTracker({maxSpeedMps: 350});
const nearby = distance => ({latitude: rx.latitude + distance / 111111, longitude: rx.longitude, support: .7});
const remote = distance => ({latitude: rx.latitude, longitude: rx.longitude + distance / 85000, support: 1});
assert.equal(hypotheses.select('T1', 1000, [remote(0), nearby(0)]).support, 1);
const selected = hypotheses.select('T1', 2000, [remote(2000), nearby(100)]);
assert.equal(selected.support, .7);assert.equal(selected.selection, 'bistatic_multi_scan');
assert.equal(selected.geometry_consistent_samples, 2);
const reversal = new BistaticHypothesisTracker({maxSpeedMps: 350, maxAccelerationMps2: 80});
assert.equal(reversal.select('reverse', 1000, [nearby(0)]).geometry_consistent_samples, 1);
assert.equal(reversal.select('reverse', 2000, [nearby(100)]).geometry_consistent_samples, 2);
assert.equal(reversal.select('reverse', 3000, [nearby(0)]).geometry_consistent_samples, 1,
  'A direction reversal must use vector acceleration, not equal speed magnitudes');
const domainTracker = new BistaticHypothesisTracker();
const domainFrame = {timestamp: 1000, sample_rate_hz: 2400000,
  capture_sample_position_available: true, capture_epoch: 0,
  producer_session_id: 'producer-a', data: [{id: 'reused', delay: 80,
    bearing_deg_true: 90, uncertainty_deg: 12,
    candidates: [{bearing_deg_true: 90, support: 1}]}]};
const support = frame => locationsFromBearing(frame, config, domainTracker)
  .data[0].geometry_consistent_samples;
assert.equal(support(domainFrame), 1);
assert.equal(support({...domainFrame, timestamp: 2000}), 2);
assert.equal(support({...domainFrame, timestamp: 2000}), 2, 'Repeated requests cannot add support');
assert.equal(support({...domainFrame, timestamp: 3000, capture_epoch: 1}), 1,
  'A new capture epoch cannot inherit diagnostic support');
assert.equal(support({...domainFrame, timestamp: 4000, capture_epoch: 1,
  producer_session_id: 'producer-b'}), 1, 'Producer sessions cannot share track-ID history');
assert.equal(support({...domainFrame, timestamp: 5000, server_session_id: 'server-a'}), 1);
assert.equal(support({...domainFrame, timestamp: 6000, server_session_id: 'server-b'}), 1);
const exactDomain = {...domainFrame, capture_epoch: 9007199254740992,
  input_lineage: {available: true, captureEpochExact: '9007199254740993'}};
assert.equal(support({...exactDomain, timestamp: 7000}), 1);
assert.equal(support({...exactDomain, timestamp: 8000}), 2);
assert.equal(support({...exactDomain, timestamp: 9000,
  input_lineage: {available: true, captureEpochExact: '9007199254740994'}}), 1,
  'Exact epochs distinguish domains even when compatibility numbers round');
domainTracker.reset();
assert.equal(support({...domainFrame, timestamp: 10000}), 1);
console.log('Bistatic bearing geolocation: diagnostic-only altitude ambiguity, native replay rate, malformed uncertainty and strict nonoperational output passed.');
