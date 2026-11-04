'use strict';
const assert = require('assert/strict');
const {IlluminatorFrameHistory} = require('./illuminator-frame-history');
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
console.log('Bounded illuminator frame history tests passed.');
