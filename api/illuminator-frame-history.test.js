'use strict';
const assert = require('assert/strict');
const {IlluminatorFrameHistory} = require('./presentation-history');
const frame = (id, timestamp, first = timestamp * 10) => ({id, timestamp,
  first_input_sample: first, end_input_sample: first + 9});
const history = new IlluminatorFrameHistory({maxFramesPerIlluminator: 2, maxBytes: 100});
history.add(frame('a', 1), 10); history.add(frame('a', 2), 10); history.add(frame('a', 3), 10);
assert.equal(history.latest('a').timestamp, 3);
assert.equal(history.exact('a', {timestamp: 1}), null);
assert.equal(history.exact('a', {timestamp: 2}).first_input_sample, 20);
assert.equal(history.exact('a', {firstInputSample: 30, endInputSample: 39}).timestamp, 3);
history.add(frame('a', 3, 31), 11);
assert.equal(history.exact('a', {timestamp: 3, firstInputSample: 31}).end_input_sample, 40);
history.add(frame('b', 4), 90);
assert.equal(history.latest('a'), null, 'global byte bound evicts the oldest lane');
assert.equal(history.latest('b').timestamp, 4);
assert.throws(() => new IlluminatorFrameHistory({maxFramesPerIlluminator: 0}));
const cohorts = new IlluminatorFrameHistory({maxFramesPerIlluminator: 4});
const aligned = (id, timestamp) => ({...frame(id, timestamp),
  capture_sample_position_available: true, capture_epoch: 3,
  capture_first_input_sample: 1000, capture_end_input_sample: 2000});
cohorts.add(aligned('a', 10)); cohorts.add(aligned('b', 11));
assert.deepEqual(cohorts.latestCohort(['a', 'b']).map(item => item.id), ['a', 'b']);
cohorts.add({...aligned('b', 12), capture_epoch: 4});
assert.deepEqual(cohorts.latestCohort(['a', 'b']).map(item => item.timestamp), [10, 11],
  'cohort selection uses capture provenance rather than unrelated latest frames');
const display = new IlluminatorFrameHistory({maxFramesPerIlluminator: 4});
display.add({...frame('a', 20), map: {data: [[1]]}, map_json: '{"data":[[1]]}',
  detections: {timestamp: 20, delay: [1]}, tracks: {timestamp: 20, data: [{id: 'old'}]}});
display.add({...frame('a', 21), map: {data_omitted: true}, map_json:
  '{"data_omitted":true}', detections: {timestamp: 21, delay: [2]},
  tracks: {timestamp: 21, data: [{id: 'new'}]}});
assert.equal(display.latest('a').presentation_frame.map_json, '{"data":[[1]]}',
  'compact current products retain the most recent full display matrix');
assert.equal(display.latest('a').presentation_frame.timestamp, 20,
  'retained display maps preserve their own product timestamp');
assert.equal(display.latest('a').map.data_omitted, true,
  'current map metadata remains distinct from retained display data');
assert.equal(display.latest('a').presentation_frame.detections.timestamp, 20);
assert.equal(display.latest('a').presentation_frame.tracks.timestamp, 20,
  'retained display overlays remain bound to the exact retained heatmap frame');
const boundedDisplay = new IlluminatorFrameHistory({maxFramesPerIlluminator: 1,
  maxBytes: 30});
boundedDisplay.add({...frame('a', 30), map: {data: [[1]]},
  map_json: '{"data":[[1]]}'}, 10);
boundedDisplay.add({...frame('a', 31), map: {data_omitted: true},
  map_json: '{"data_omitted":true}'}, 20);
assert.equal(boundedDisplay.latest('a'), null,
  'retained full-map bytes participate in the global history bound');
cohorts.clear();
assert.equal(cohorts.latest('a'), null, 'a new producer session discards prior cohorts');
assert.equal(cohorts.bytes, 0);
console.log('Bounded illuminator frame history tests passed.');
