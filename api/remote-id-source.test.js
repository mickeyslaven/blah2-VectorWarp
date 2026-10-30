'use strict';

const assert = require('assert');
const {normalizeFeed, sourceUrl, createRemoteIdSource} = require('./remote-id-source');

const feed = {schema_version: 1, now: 1000, messages: 12, drones: [{
  id: '1581F-test', id_type: 'serial', ua_type: 'Helicopter/Multirotor',
  lat: 40.45, lon: -80.05, alt_geom_ft: 620, agl_ft: 220, gs: 31,
  track: 92, geom_rate: 120, rssi: -61, seen: .1, seen_pos: .2,
  rid_source: 'ble', rid_sources: ['ble', 'wifi_beacon'], self_id: 'Survey',
  operator: {lat: 40.44, lon: -80.06, location_type: 'live_gnss', seen: .4}
}]};

assert.equal(sourceUrl('127.0.0.1:8754').href,
  'http://127.0.0.1:8754/data/remoteid.json');
assert.throws(() => sourceUrl('http://user:pass@example.test'), /without credentials/);
const normalized = normalizeFeed(feed, {source: 'local'});
assert.equal(normalized.aircraft.length, 1);
assert.equal(normalized.aircraft[0].position_source, 'REMOTE_ID');
assert.equal(normalized.aircraft[0].category, 'B6');
assert.equal(normalized.aircraft[0].remote_id.operator.location_type, 'live_gnss');

const config = {truth: {remote_id: {enabled: true, source: '127.0.0.1:8754',
  poll_interval: .1, max_position_age: 30, smoothing_window: 2}},
capture: {fc: 509000000}, location: {rx: {latitude: 40.44, longitude: -80.06, altitude: 300},
  tx: {latitude: 40.49, longitude: -80.00, altitude: 600}}};
let reads = 0;
const source = createRemoteIdSource(config, {fetchJson: async () => { reads++; return feed; },
  now: () => 1000000});
source.get('aircraft').then(async value => {
  assert.equal(value.aircraft[0].flight, '1581F-test');
  assert.equal((await source.status()).online, true);
  await source.get('aircraft');
  assert.equal(reads, 1);
  console.log('remote-id-source tests passed');
}).catch(error => { console.error(error); process.exitCode = 1; });
