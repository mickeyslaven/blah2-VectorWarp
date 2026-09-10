'use strict';
const assert = require('assert');
const {ecef, distance, DelayDopplerHistory} = require('./adsb-geometry');
const C = 299792458;
const config = {capture: {fc: 100000000}, truth: {adsb: {smoothing_window: 3, max_position_age: 10}},
  location: {rx: {latitude: 0, longitude: 0, altitude: 0}, tx: {latitude: 0, longitude: 1, altitude: 0}}};
assert.ok(Math.abs(ecef(0, 0, 0)[0] - 6378137) < 1e-6, 'WGS84 equatorial radius');
assert.ok(Math.abs(distance(ecef(0, 0, 0), ecef(0, 1, 0)) - 111318.0779) < .2, 'WGS84 one-degree chord');
const history = new DelayDopplerHistory(config);
const point = (now, lon, seen = 0, extra = {}) => ({now, aircraft: [{hex: 'abc', flight: 'TEST', lat: 0, lon, alt_geom: 10000, seen_pos: seen, ...extra}]});
assert.deepEqual(history.update(point(100, .5)).data, {}, 'First position warms up');
const first = history.update(point(101, .6)).data.abc;
assert.ok(Number.isFinite(first.delay) && Number.isFinite(first.doppler));
assert.ok(first.doppler < 0, 'Increasing bistatic range has negative Doppler');
const delayed = history.update(point(103, .7, 1)).data.abc;
const rate = -(delayed.doppler) * (C / config.capture.fc);
assert.ok(Number.isFinite(rate) && rate > 0, 'Doppler is numeric Hz derived from metres/second');
assert.deepEqual(history.update(point(104, .7)).data.abc, delayed,
  'PR #5 duplicate coordinates retain the last computed overlay');
assert.deepEqual(history.update(point(102, .8)).data.abc, delayed, 'Out-of-order position is ignored');
assert.deepEqual(history.update(point(120, .9, 11)).data, {}, 'Stale positions and retained output are evicted');
const invalid = new DelayDopplerHistory(config);
for (const extra of [{lat: 91}, {lon: 181}, {alt_geom: undefined}, {alt_geom: Number.MAX_VALUE}, {seen_pos: NaN}])
  assert.deepEqual(invalid.update(point(100, .5, 0, extra)).data, {}, 'Invalid metadata is ignored');
const unusableSite = new DelayDopplerHistory({...config, location: {rx: {latitude: 0, longitude: 0, altitude: Number.MAX_VALUE}, tx: config.location.tx}});
assert.deepEqual(unusableSite.update(point(100, .5)).data, {}, 'Nonfinite intermediate site geometry is never emitted');
const bounded = new DelayDopplerHistory(config);
for (let i = 0; i < 20; i++) bounded.update(point(100 + i, .1 + i / 100));
assert.ok(bounded.history.get('abc').length <= 4, 'History is bounded by smoothing window');
bounded.configure({...config, capture: {fc: 200000000}});
assert.equal(bounded.history.size, 0, 'Configuration change resets geometry history');
console.log('ADS-B WGS84 geometry, units, PR #5 timing/duplicates, ordering, eviction and reset tests passed.');
