'use strict';

const assert = require('assert');
const {recordingReplayTopology} = require('./recording-replay-topology');

assert.deepEqual(recordingReplayTopology({capture: {device: {type: 'Usrp'}},
  process: {reference_synthesis: {mode: 'dedicated', channels: [1]}}}), {
  channel_count: 2, reference_channel: 1, surveillance_channels: [0]
}, 'B210 recordings preserve the scientific reference/surveillance wiring');

assert.deepEqual(recordingReplayTopology({capture: {device: {type: 'Kraken',
  channel_count: 5, reference_channel: 2, surveillance_channels: [0, 1, 3, 4]}},
  process: {reference_synthesis: {mode: 'dedicated', channels: [2]}}}), {
  channel_count: 5, reference_channel: 2, surveillance_channels: [0, 1, 3, 4]
}, 'Explicit Kraken topology remains authoritative');

for (const type of ['Usrp', 'RspDuo']) {
  assert.deepEqual(recordingReplayTopology({capture: {device: {type}},
    process: {reference_synthesis: {mode: 'array_eigenbeam', channels: [0, 1]}}}), {
    channel_count: 2, reference_channel: 0, surveillance_channels: [0, 1]
  }, `${type} array recordings preserve both surveillance inputs`);
  assert.deepEqual(recordingReplayTopology({capture: {device: {type,
    channel_count: 5, reference_channel: 4, surveillance_channels: [0, 2, 3]}},
    process: {reference_synthesis: {mode: 'dedicated', channels: [1]}}}), {
    channel_count: 2, reference_channel: 1, surveillance_channels: [0]
  }, `${type} recordings ignore dormant live Kraken roles`);
}

console.log('Recording replay topology passed.');
