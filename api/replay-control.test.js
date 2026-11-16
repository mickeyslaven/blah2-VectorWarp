'use strict';
const assert = require('node:assert/strict');
const {controlsAvailable, updateControl} = require('./replay-control');
const {status} = require('./processor-status');
const processor = {input: 'replay', receiver: 'Kraken', state: 'playing', error: '',
  replayControlsAvailable: true, file: '/replay.mchq', sampleRate: 100,
  frequency: 100000000, positionSamples: 20, totalSamples: 100, loops: 0,
  trailingSamples: 0, recording: false, recordingFile: '', recordingError: '', recordedSamples: 0};
const previous = {paused: false, seekSample: 0, revision: 7};
assert.equal(status(processor).value.replayControlsAvailable, true);
assert.equal(status({...processor, replayControlsAvailable: undefined}).value.replayControlsAvailable, false);
for (const state of ['playing', 'paused', 'draining', 'complete'])
  assert.equal(controlsAvailable({...processor, state}), true);
for (const unavailable of [null, {...processor, input: 'live'},
  {...processor, replayControlsAvailable: false}, {...processor, replayControlsAvailable: undefined},
  ...['loading', 'stopped', 'error'].map(state => ({...processor, state}))]) {
  assert.equal(controlsAvailable(unavailable), false);
  for (const action of ['play', 'pause', 'seek', 'skip'])
    assert.equal(updateControl(unavailable, previous, {action, seconds: 0}).status, 409);
}
assert.deepEqual(updateControl(processor, previous, {action: 'pause'}).control,
  {...previous, paused: true});
assert.deepEqual(updateControl({...processor, state: 'complete'}, previous, {action: 'play'}).control,
  {paused: false, seekSample: 0, revision: 8});
assert.equal(updateControl({...processor, state: 'complete'}, previous, {action: 'seek', seconds: .5}).control.seekSample, 50);
assert.equal(updateControl(processor, previous, {action: 'skip', seconds: -.1}).control.seekSample, 10);
assert.equal(updateControl(processor, previous, {action: 'seek', seconds: Infinity}).status, 400);
assert.equal(updateControl(processor, previous, {action: 'invalid'}).status, 400);
assert.deepEqual(previous, {paused: false, seekSample: 0, revision: 7});
console.log('Replay control availability, EOF restart, and validation checks passed');
