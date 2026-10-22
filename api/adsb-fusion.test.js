'use strict';

const assert = require('assert');
const {separationKm, projectAircraft, fuseAircraftData} = require('./adsb-fusion');

const nowMs = 2_000_000_000_000;
const snapshot = (source, now, aircraft) => ({source, receivedAtMs: nowMs, data: {now, aircraft}});
const local = {id: 'local-readsb', kind: 'file', reliability: 1};
const publicA = {id: 'adsb-fi', kind: 'http', reliability: .75};
const publicB = {id: 'licensed-backup', kind: 'http', authenticated: true};

assert.ok(separationKm({lat: 40, lon: -80}, {lat: 40.01, lon: -80}) > 1);
const projected = projectAircraft({lat: 40, lon: -80, gs: 360, track: 90,
  alt_geom: 10000, geom_rate: 600}, 2);
assert.equal(projected.projected, true);
assert.ok(projected.aircraft.lon > -80 && Math.abs(projected.aircraft.lat - 40) < 1e-9);
assert.equal(projected.aircraft.alt_geom, 10020);

const single = fuseAircraftData([snapshot(local, nowMs / 1000 - .2, [
  {hex: 'ABC123', lat: 40, lon: -80, alt_geom: 10000, gs: 100, track: 90,
    seen_pos: 1.5, nic: 9}
])], {nowMs});
assert.equal(single.now, nowMs / 1000);
assert.equal(single.aircraft[0].seen_pos, 0,
  'a moving observation is propagated to fused CPI time');
assert.ok(Math.abs(single.aircraft[0].vectorwarp.observation_position_timestamp_s -
  (nowMs / 1000 - 1.7)) < 1e-6);
assert.ok(Math.abs(single.aircraft[0].vectorwarp.observation_position_age_s - 1.7) < 1e-6);
assert.equal(single.aircraft[0].vectorwarp.position_timestamp_s, nowMs / 1000);
assert.deepEqual(single.aircraft[0].vectorwarp.agreeing_sources, ['local-readsb']);

const stale = fuseAircraftData([snapshot(local, nowMs / 1000, [
  {hex: 'abc123', lat: 40, lon: -80, alt_geom: 10000, seen_pos: 31}
])], {nowMs, maxPositionAgeSeconds: 30});
assert.deepEqual(stale.aircraft, [], 'stale individual positions are rejected despite a fresh feed');

const consensus = fuseAircraftData([
  snapshot(local, nowMs / 1000, [
    {hex: 'abc123', lat: 41, lon: -80, alt_geom: 10000, seen_pos: .1}]),
  snapshot(publicA, nowMs / 1000, [
    {hex: 'abc123', lat: 40, lon: -80, alt_geom: 10020, seen_pos: .5}]),
  snapshot(publicB, nowMs / 1000, [
    {hex: 'abc123', lat: 40.001, lon: -80, alt_geom: 10010, seen_pos: .7}])
], {nowMs, disagreementKm: 3});
assert.equal(consensus.aircraft[0].lat, 41,
  'the freshest event wins even when older sources form a different consensus');
assert.deepEqual(consensus.aircraft[0].vectorwarp.agreeing_sources, ['local-readsb']);
assert.deepEqual(new Set(consensus.aircraft[0].vectorwarp.rejected_sources),
  new Set(['adsb-fi', 'licensed-backup']));
assert.equal(consensus.aircraft[0].vectorwarp.disagreement, true);

const duplicate = fuseAircraftData([
  snapshot(publicA, nowMs / 1000 - 1, [
    {hex: 'abc123', lat: 39, lon: -80, alt_geom: 9000, seen_pos: 1}]),
  snapshot(publicA, nowMs / 1000, [
    {hex: 'abc123', lat: 40, lon: -80, alt_geom: 10000, seen_pos: .2}])
], {nowMs});
assert.equal(duplicate.aircraft[0].lat, 40, 'latest observation wins within one source');

const newestPreferred = fuseAircraftData([
  snapshot({id: 'pi-zero', kind: 'http', authoritative: true, reliability: 1}, nowMs / 1000, [
    {hex: 'abc123', lat: 40, lon: -80, alt_geom: 10000, seen_pos: .3}]),
  snapshot({id: 'online-a', kind: 'exchange', reliability: .65}, nowMs / 1000, [
    {hex: 'abc123', lat: 41, lon: -80, alt_geom: 10000, seen_pos: .1}]),
  snapshot({id: 'online-b', kind: 'exchange', reliability: .65}, nowMs / 1000, [
    {hex: 'abc123', lat: 41.001, lon: -80, alt_geom: 10000, seen_pos: .2}])
], {nowMs, disagreementKm: 3});
assert.equal(newestPreferred.aircraft.length, 1, 'all sources collapse to one aircraft per ICAO hex');
assert.equal(newestPreferred.aircraft[0].lat, 41,
  'the observation with the newest actual position timestamp wins across sources');
assert.equal(newestPreferred.aircraft[0].vectorwarp.source_id, 'online-a');

console.log('PASS ADS-B event-time normalization, freshness, deduplication and consensus fusion');
