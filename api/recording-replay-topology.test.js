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

console.log('Recording replay topology passed.');
