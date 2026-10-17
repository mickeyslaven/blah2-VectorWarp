'use strict';

const assert = require('assert');
const {EventEmitter} = require('events');
const {TrackerForwarder, createTrackerForwarder} = require('./tracker-forward');

class Socket extends EventEmitter {
  constructor() { super(); this.writes = []; }
  write(value) { this.writes.push(value); return true; }
  setNoDelay() {}
  destroy() { this.emit('close'); }
}

const tick = () => new Promise(resolve => setImmediate(resolve));
const config = {network: {tracker_forward: {enabled: true, host: '127.0.0.1', port: 30100}},
  truth: {adsb: {delay_tolerance: 2, doppler_tolerance: 5}}};
const feed = {now: 10, aircraft: [{hex: 'ABC123', flight: 'TEST1', lat: 40, lon: -80,
  alt_baro: 10000, alt_geom: 10100, gs: 300, track: 90, geom_rate: 0}]};
const predictions = {abc123: {flight: 'TEST1', delay: 12, doppler: -40}};

async function forwardsAnnotatedDelimitedFrames() {
  const socket = new Socket();
  const source = {get: async kind => kind === 'aircraft' ? feed : predictions};
  const forwarder = new TrackerForwarder(config, source, {connect: () => socket});
  socket.emit('connect');
  forwarder.accept(JSON.stringify(
    {timestamp: 1, delay: [12.5, 30], doppler: [-38, 10], snr: [8, 9]}));
  await tick(); await tick();
  assert.equal(socket.writes.length, 1);
  assert.ok(socket.writes[0].endsWith('\n'));
  const value = JSON.parse(socket.writes[0]);
  assert.equal(value.timestamp, 1);
  assert.equal(value.adsb[0].hex, 'abc123');
  assert.equal(value.adsb[0].expected_delay, 12);
  assert.equal(value.adsb[1], null);
  assert.equal(forwarder.status().matches, 1);
  assert.equal(forwarder.status().unmatched, 1);
  forwarder.close();
}

async function keepsOnlyNewestFrameWhileTruthIsSlow() {
  const socket = new Socket();
  let release;
  const blocked = new Promise(resolve => { release = resolve; });
  const source = {get: async kind => { await blocked; return kind === 'aircraft' ? feed : predictions; }};
  const forwarder = new TrackerForwarder(config, source, {connect: () => socket});
  socket.emit('connect');
  forwarder.accept({timestamp: 1, delay: [], doppler: [], snr: []});
  forwarder.accept({timestamp: 2, delay: [], doppler: [], snr: []});
  forwarder.accept({timestamp: 3, delay: [], doppler: [], snr: []});
  assert.equal(forwarder.status().overwritten, 1);
  release();
  await tick(); await tick(); await tick();
  assert.equal(socket.writes.length, 2);
  assert.equal(JSON.parse(socket.writes[0]).timestamp, 1);
  assert.equal(JSON.parse(socket.writes[1]).timestamp, 3);
  forwarder.close();
}

async function disabledDoesNothing() {
  let connected = false, fetched = false;
  const forwarder = new TrackerForwarder({network: {}, truth: {adsb: {}}},
    {get: async () => { fetched = true; }}, {connect: () => { connected = true; }});
  forwarder.accept({timestamp: 1});
  await tick();
  assert.equal(connected, false);
  assert.equal(fetched, false);
  assert.equal(forwarder.status().enabled, false);
}

async function rejectsIncompleteFrames() {
  const socket = new Socket();
  const source = {get: async () => assert.fail('Invalid frames must not query ADS-B')};
  const forwarder = new TrackerForwarder(config, source, {connect: () => socket});
  socket.emit('connect');
  forwarder.accept('{invalid');
  forwarder.accept({delay: [], doppler: [], snr: []});
  forwarder.accept({timestamp: 1, delay: [], doppler: []});
  await tick();
  assert.equal(socket.writes.length, 0);
  assert.equal(forwarder.status().frames, 3);
  assert.equal(forwarder.status().invalid, 3);
  forwarder.close();
}

async function mirrorsFramesWithoutAdsbToAnIndependentShadow() {
  const sockets = [];
  let truthReads = 0;
  const dual = structuredClone(config);
  dual.network.tracker_forward.unassisted =
    {enabled: true, host: '127.0.0.1', port: 30101};
  const forwarder = createTrackerForwarder(dual,
    {get: async kind => {
      truthReads += 1;
      return kind === 'aircraft' ? feed : predictions;
    }}, {connect: () => {
      const socket = new Socket(); sockets.push(socket); return socket;
    }});
  assert.equal(sockets.length, 2);
  sockets.forEach(socket => socket.emit('connect'));
  forwarder.accept({timestamp: 1, delay: [12.5], doppler: [-38], snr: [8]});
  await tick(); await tick();
  assert.equal(sockets[0].writes.length, 1);
  assert.equal(sockets[1].writes.length, 1);
  assert.equal(JSON.parse(sockets[0].writes[0]).adsb[0].hex, 'abc123');
  assert.deepEqual(JSON.parse(sockets[1].writes[0]).adsb, [null]);
  assert.equal(truthReads, 2, 'Only the assisted sink may read ADS-B truth');
  assert.equal(forwarder.status().unassisted.forwarded, 1);
  assert.equal(forwarder.status().unassisted.matches, 0);
  forwarder.close();
}

(async () => {
  await forwardsAnnotatedDelimitedFrames();
  await keepsOnlyNewestFrameWhileTruthIsSlow();
  await disabledDoesNothing();
  await rejectsIncompleteFrames();
  await mirrorsFramesWithoutAdsbToAnIndependentShadow();
  console.log('tracker-forward tests passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
