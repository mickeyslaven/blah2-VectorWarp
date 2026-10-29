'use strict';

const assert = require('node:assert/strict');
const {radarCoverageFromPasses, radarCoverageFromAccumulator,
  rememberRadarCoverage} = require('./evidence-statistics');

const now = 2_000_000_000_000;
const sample = (timestamp, bearing, range, altitude, latitude, longitude) => ({
  timestamp_ms: timestamp, bearing_deg_true: bearing, ground_range_km: range,
  altitude_geom_ft: altitude, latitude, longitude
});
const pass = (id, hex, timestamp, rows, tracked = true) => ({
  id, hex, siteKey: 'site', startedAt: timestamp - 1000, lastAt: timestamp,
  truthHistory: rows,
  cpiAudit: rows.map(row => ({timestamp_ms: row.timestamp_ms, stable_tracked: tracked}))
});

const rows = [
  pass('near', 'abc001', now - 1000, [sample(now - 1000, 2, 25, 8000, 40.2, -80.0)]),
  pass('far', 'abc002', now - 900, [sample(now - 900, 3, 52, 12000, 40.4, -80.1)]),
  pass('other-bearing', 'abc003', now - 800, [sample(now - 800, 94, 31, 33000, 40.1, -79.5)]),
  pass('feed-only', 'abc004', now - 700, [sample(now - 700, 180, 300, 15000, 38, -80)], false),
  pass('expired', 'abc005', now - 25 * 3600000,
    [sample(now - 25 * 3600000, 270, 400, 15000, 40, -84)])
];

const all = radarCoverageFromPasses(rows, {now, hours: 24, bins: 72, siteKey: 'site'});
assert.equal(all.supporting_passes, 3);
assert.equal(all.supporting_aircraft, 3);
assert.equal(all.points.length, 2, 'same bearing bin keeps only the farthest radar-confirmed point');
assert.equal(all.points.find(point => point.bin === 0).pass_id, 'far');
assert.ok(!all.points.some(point => point.pass_id === 'feed-only'),
  'ADS-B reach without a confirmed radar association cannot expand coverage');

const low = radarCoverageFromPasses(rows, {now, altitudeBand: 'low', siteKey: 'site'});
assert.deepEqual(low.points.map(point => point.pass_id), ['near']);
const veryHigh = radarCoverageFromPasses(rows, {now, altitudeBand: 'very_high', siteKey: 'site'});
assert.deepEqual(veryHigh.points.map(point => point.pass_id), ['other-bearing']);

const state = {};
rememberRadarCoverage(state, rows[0], rows[0].truthHistory[0]);
rememberRadarCoverage(state, rows[1], rows[1].truthHistory[0]);
rememberRadarCoverage(state, rows[2], rows[2].truthHistory[0]);
const accumulated = radarCoverageFromAccumulator(state.radarCoverage, {now, hours: 24});
assert.equal(accumulated.points.length, 2);
assert.equal(accumulated.points.find(point => point.bin === 0).pass_id, 'far');
assert.equal(accumulated.supporting_passes, 3);
assert.deepEqual(radarCoverageFromAccumulator(state.radarCoverage,
  {now, altitudeBand: 'low'}).points.map(point => point.pass_id), ['near']);

console.log('Radar-confirmed rolling coverage excludes feed-only reach and respects altitude filters.');
