'use strict';
const assert = require('assert');
const {portableRecordingBytesPerSecond, manualRecordingExpired} = require('./recording-rate');

assert.equal(portableRecordingBytesPerSecond({capture: {fs: 24000000,
  device: {type: 'Usrp'}}}), 384000000);
assert.equal(portableRecordingBytesPerSecond({capture: {fs: 2400000,
  device: {type: 'Kraken', channel_count: 5}}}), 96000000);
assert.equal(portableRecordingBytesPerSecond({capture: {fs: 2000000,
  device: {type: 'RspDuo'}}}), 32000000);
assert.equal(portableRecordingBytesPerSecond({capture: {fs: 0,
  device: {type: 'HackRF'}}}), 0);
assert.equal(manualRecordingExpired({}, 1000, 300999), false);
assert.equal(manualRecordingExpired({}, 1000, 301000), true);
assert.equal(manualRecordingExpired({save: {retention: {maximum_manual_seconds: 60}}},
  1000, 60999), false);
assert.equal(manualRecordingExpired({save: {retention: {maximum_manual_seconds: 60}}},
  1000, 61000), true);
console.log('Portable recording disk-rate estimates passed.');
