'use strict';

const assert = require('assert');
const {status, fresh} = require('./processor-status');

const replay = {
  input: 'replay', receiver: 'Kraken', state: 'playing', error: '',
  file: '/recordings/pass.mchq', sampleRate: 2400000, frequency: 527000000,
  positionSamples: 120, totalSamples: 240, loops: 2, trailingSamples: 8,
  recording: false, recordingFile: '', recordingError: '', recordedSamples: 0
};
assert.equal(status(replay).valid, true);
assert.equal(status({...replay, state: 'error', error: 'truncated input'}).value.state, 'error');
assert.equal(status({...replay, state: 'error', error: 'writer failed\ncheck disk'}).value.error,
  'writer failed check disk');
for (const changed of [
  {...replay, receiver: 'other'}, {...replay, state: 'unknown'},
  {...replay, positionSamples: -1}, {...replay, positionSamples: 241},
  {...replay, recording: true}, {...replay, state: 'error', error: ''},
  {...replay, file: 'x'.repeat(4097)}
]) assert.equal(status(changed).valid, false);
assert.equal(fresh({receivedAt: 1000}, 5999), true);
assert.equal(fresh({receivedAt: 1000}, 6001), false);
console.log('Processor status validation and freshness tests passed.');
