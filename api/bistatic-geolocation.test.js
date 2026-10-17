'use strict';
const assert = require('node:assert/strict');
const {inverse, locate, BistaticHypothesisTracker, locationsFromBearing} = require('./bistatic-geolocation');
const rx = {latitude: 40.442030432817, longitude: -80.056697229925};
const tx = {latitude: 40.49388889, longitude: -80.01888889};
const baseline = inverse(rx, tx);
assert.ok(baseline.distanceM > 6000 && baseline.distanceM < 7000);
const along = locate(rx, tx, 1000, baseline.bearingDeg);
assert.ok(along && along.rangeM > baseline.distanceM);
const opposite = locate(rx, tx, 1000, baseline.bearingDeg + 180);
assert.ok(opposite && opposite.rangeM < along.rangeM);
const config = {capture: {fs: 2400000}, process: {bearing: {enable: true}}, location: {rx, tx}};
const result = locationsFromBearing({timestamp: 1, data: [{id: 'A001', delay: 80,
  bearing_deg_true: 90, uncertainty_deg: 12, resolved: true,
  candidates: [{bearing_deg_true: 90, support: .8}]}]}, config);
assert.equal(result.data.length, 1);
assert.ok(result.data[0].location && result.data[0].uncertainty_corridor.length === 3);
assert.deepEqual(result.data[0].estimate, result.data[0].location);
const unresolved = locationsFromBearing({timestamp: 2, data: [{id: 'A002', delay: 80,
  state: 'COASTING', bearing_deg_true: 135, uncertainty_deg: 40, resolved: false,
  candidates: [{bearing_deg_true: 90, support: .55}]}]}, config);
assert.equal(unresolved.data[0].location, null);
assert.ok(unresolved.data[0].estimate);
assert.equal(unresolved.data[0].uncertainty_corridor.length, 0);
assert.equal(locationsFromBearing({data: []}, {...config, process: {bearing: {enable: false}}}).enabled, false);
const hypotheses = new BistaticHypothesisTracker({maxSpeedMps: 350});
const nearby = distance => ({latitude: rx.latitude + distance / 111111, longitude: rx.longitude, support: .7});
const remote = distance => ({latitude: rx.latitude, longitude: rx.longitude + distance / 85000, support: 1});
assert.equal(hypotheses.select('T1', 1000, [remote(0), nearby(0)]).support, 1);
const selected = hypotheses.select('T1', 2000, [remote(2000), nearby(100)]);
assert.equal(selected.support, .7);assert.equal(selected.selection, 'bistatic_multi_scan');
assert.equal(selected.geometry_consistent_samples, 2);
console.log('Bistatic bearing geolocation: strict fixes, diagnostic estimates, candidates, uncertainty, and disabled path passed.');
