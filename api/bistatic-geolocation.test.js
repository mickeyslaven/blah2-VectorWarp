'use strict';
const assert = require('node:assert/strict');
const {inverse, locate, locationsFromBearing} = require('./bistatic-geolocation');
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
assert.equal(locationsFromBearing({data: []}, {...config, process: {bearing: {enable: false}}}).enabled, false);
console.log('Bistatic bearing geolocation: formula, candidates, uncertainty, and disabled path passed.');
