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
const stages = Object.fromEntries(['open', 'apiVersion', 'lock', 'enumerate',
  'select', 'unlock', 'debugEnable', 'getDeviceParams', 'init', 'gainUpdateA',
  'gainUpdateB'].map(name => [name, true]));
const startup = {
  schema: 1, receiver: 'RspDuo', status: 'accepted', hardwareVerified: false,
  readbackAvailable: false,
  requested: {serial: 'mock-only', frequency: 204640000, sampleRate: 2000000,
    agcSetPoint: -30, bandwidthNumber: 50, gainReduction: [30, 31],
    lnaState: 3, dabNotch: true, rfNotch: false,
    ifBandwidthKhz: 1536, ifFrequencyKhz: 1620, decimation: 1},
  selected: {serial: 'mock-only', deviceIndex: 0, hardwareVersion: 3,
    tuner: 'Both', mode: 'Dual_Tuner'},
  sdk: {version: 3.15, stages}
};
const live = {...replay, input: 'live', receiver: 'RspDuo', state: 'live',
  file: '', frequency: 204640000, sampleRate: 2000000,
  receiverStartup: startup};
assert.equal(status(live).valid, true);
assert.equal(status({...live, receiverStartup: undefined}).valid, true); // older module
assert.equal(status({...live, receiverStartup: {...startup, status: 'pending',
  sdk: {...startup.sdk, stages: {...stages, gainUpdateB: false}}}}).valid, true);
assert.equal(status({...live, state: 'error', error: 'Update tuner B gain failed',
  receiverStartup: {...startup, status: 'pending',
    sdk: {...startup.sdk, stages: {...stages, gainUpdateB: false}}}}).valid, true);
assert.equal(status({...live, state: 'error', error: 'Receiver disconnected'}).valid, true);
// Native validation fails before a receipt exists; keep the actionable base
// status even when direct YAML bypassed the browser's receiver constraints.
const invalidRate = {...live, sampleRate: 6000000, state: 'error',
  error: '[RspDuo] Unsupported sample rate/decimation.', receiverStartup: undefined};
assert.equal(status(invalidRate).valid, true);
assert.equal(status(invalidRate).value.error, '[RspDuo] Unsupported sample rate/decimation.');
const invalidLna = {...live, frequency: 1000000000, state: 'error',
  error: '[RspDuo] LNA state is outside the RSPduo frequency-band range.',
  receiverStartup: undefined};
assert.equal(status(invalidLna).valid, true);
for (const field of ['frequency', 'sampleRate']) {
  const failed = {...invalidRate, [field]: 0};
  assert.equal(status(failed).valid, true, `Preserve native zero-${field} startup error`);
  assert.equal(status(failed).value.state, 'error');
  assert.equal(status({...failed, state: 'live', error: ''}).valid, false);
  assert.equal(status({...failed, error: ''}).valid, false);
  assert.equal(status({...failed, [field]: -1}).valid, false);
}
for (const receiverStartup of [
  {...startup, hardwareVerified: true},
  {...startup, readbackAvailable: true},
  {...startup, status: 'accepted', sdk: {...startup.sdk, stages: {...stages, gainUpdateB: false}}},
  {...startup, requested: {...startup.requested, frequency: 204650000}},
  {...startup, selected: {...startup.selected, serial: 'other'}},
  {...startup, requested: {...startup.requested, lnaState: 9, frequency: 1000000000}},
  {...startup, selected: null}
]) assert.equal(status({...live, receiverStartup}).valid, false);
assert.equal(status({...replay, receiverStartup: startup}).valid, false);
console.log('Processor status validation and freshness tests passed.');
