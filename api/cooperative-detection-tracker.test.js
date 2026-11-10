'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {compatibleGeometry, geometryCatalog, CooperativeDetectionTracker} =
  require('./cooperative-detection-tracker');

const rx = {latitude: 35, longitude: -90, altitude: 250};
const tx = {latitude: 35.2, longitude: -89.8, altitude: 500};
const otherTx = {latitude: 34.8, longitude: -90.3, altitude: 450};
const source = (node, channel = 'channel-a', transmitter = tx) => ({
  key: `${node}:${channel}`, node, channel, receiver: rx, transmitter,
  frequency_hz: 500000000, sample_rate_hz: node === 'node-a' ? 6144000 : 2400000
});
const frame = (source, timestamp, delay = 10, doppler = -100, snr = 10) => ({
  source: source.key, frame: {timestamp, delay: [delay], doppler: [doppler], snr: [snr]}
});

test('groups only colocated receiver and transmitter geometry', () => {
  const nodeA = source('node-a'), nodeB = source('node-b');
  assert.equal(compatibleGeometry(nodeA, nodeB, 100), true);
  assert.equal(compatibleGeometry(nodeA, source('node-c', 'channel-b', otherTx), 100), false);
  const catalog = geometryCatalog([nodeA, nodeB, source('node-c', 'channel-b', otherTx)]);
  assert.equal(catalog.get(nodeA.key), catalog.get(nodeB.key));
  assert.notEqual(catalog.get(nodeA.key), catalog.get('node-c:channel-b'));
});

test('recognizes colocated geometry across the antimeridian', () => {
  const left = source('node-a'), right = source('node-b');
  left.receiver = {latitude: 0, longitude: 179.9999, altitude: 10};
  right.receiver = {latitude: 0, longitude: -179.9999, altitude: 10};
  assert.equal(compatibleGeometry(left, right, 100), true);
});

test('forms an active track only after temporal support from two nodes', () => {
  const nodeA = source('node-a'), nodeB = source('node-b');
  const tracker = new CooperativeDetectionTracker({m: 4, n: 6, deleteAfter: 3});
  let result;
  result = tracker.update([nodeA, nodeB], [frame(nodeA, 1000)]);
  assert.equal(result.tracks[0].state, 'COOPERATIVE_CANDIDATE');
  result = tracker.update([nodeA, nodeB], [frame(nodeB, 1500, 10.02)]);
  assert.equal(result.tracks[0].state, 'COOPERATIVE_CANDIDATE');
  result = tracker.update([nodeA, nodeB], [frame(nodeA, 2000, 10.04)]);
  assert.equal(result.tracks[0].state, 'COOPERATIVE_CANDIDATE');
  result = tracker.update([nodeA, nodeB], [frame(nodeB, 2500, 10.06)]);
  assert.equal(result.tracks[0].state, 'COOPERATIVE_ACTIVE');
  assert.deepEqual(result.tracks[0].contributing_nodes, ['node-b']);
  assert.deepEqual(result.tracks[0].qualification_nodes, ['node-a', 'node-b']);
  assert.equal(result.tracks[0].supporting_cpis, 4);
});

test('does not promote a single-node track', () => {
  const nodeA = source('node-a'), nodeB = source('node-b');
  const tracker = new CooperativeDetectionTracker({m: 4, n: 6});
  let result;
  for (let index = 0; index < 6; index += 1)
    result = tracker.update([nodeA, nodeB], [frame(nodeA, 1000 + index * 500, 10 + index * .01)]);
  assert.equal(result.tracks[0].state, 'COOPERATIVE_CANDIDATE');
  assert.deepEqual(result.tracks[0].contributing_nodes, ['node-a']);
});

test('keeps a qualified cooperative track active through a one-node coverage interval', () => {
  const nodeA = source('node-a'), nodeB = source('node-b');
  const tracker = new CooperativeDetectionTracker({m: 2, n: 3, deleteAfter: 3});
  tracker.update([nodeA, nodeB], [frame(nodeA, 1000)]);
  let result = tracker.update([nodeA, nodeB], [frame(nodeB, 1500, 10.01)]);
  assert.equal(result.tracks[0].state, 'COOPERATIVE_ACTIVE');
  result = tracker.update([nodeA, nodeB], [frame(nodeA, 2000, 10.02)]);
  result = tracker.update([nodeA, nodeB], [frame(nodeA, 2500, 10.03)]);
  result = tracker.update([nodeA, nodeB], [frame(nodeA, 3000, 10.04)]);
  assert.equal(result.tracks[0].state, 'COOPERATIVE_ACTIVE');
  assert.deepEqual(result.tracks[0].contributing_nodes, ['node-a']);
  assert.deepEqual(result.tracks[0].qualification_nodes, ['node-a', 'node-b']);
});

test('does not combine detections from distinct transmitter geometry', () => {
  const nodeA = source('node-a'), other = source('node-b', 'channel-b', otherTx);
  const tracker = new CooperativeDetectionTracker({m: 2, n: 3});
  let result = tracker.update([nodeA, other], [frame(nodeA, 1000), frame(other, 1000)]);
  assert.equal(result.tracks.length, 2);
  result = tracker.update([nodeA, other], [frame(nodeA, 1500), frame(other, 1500)]);
  assert.equal(result.tracks.every(track => track.state === 'COOPERATIVE_CANDIDATE'), true);
});

test('rejects replayed source timestamps and expires stale publication', () => {
  const nodeA = source('node-a');
  const tracker = new CooperativeDetectionTracker({maximumCoastSeconds: 2});
  const first = tracker.update([nodeA], [frame(nodeA, 1000)], 10000);
  const replay = tracker.update([nodeA], [frame(nodeA, 1000)], 10500);
  assert.equal(replay.tracks[0].supporting_cpis, first.tracks[0].supporting_cpis);
  assert.equal(tracker.snapshot(13001).available, false);
});
