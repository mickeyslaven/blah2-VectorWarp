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
    ifBandwidthKhz: 1536, ifFrequencyKhz: 1620, decimation: 1,
    usbMode: 'isoch', sampleCounterScale: 1, sdkDebugLevel: 'error'},
  selected: {serial: 'mock-only', deviceIndex: 0, hardwareVersion: 3,
    tuner: 'Both', mode: 'Dual_Tuner'},
  sdk: {version: 3.15, parameters: {adcSampleRateHz: 6000000,
    descriptorSampleRateHz: 6000000}, stages}
};
const live = {...replay, input: 'live', receiver: 'RspDuo', state: 'live',
  file: '', frequency: 204640000, sampleRate: 2000000,
  receiverStartup: startup};
assert.equal(status(live).valid, true);
assert.equal(status({...live, receiverStartup: undefined}).valid, true); // older module
const clipAcknowledgement = status({...live, receiverStartup: undefined,
  clipRevision: 7, clipError: '', clipFile: '/clips/pass.mchq',
  clipSamples: 12000000});
assert.equal(clipAcknowledgement.valid, true);
assert.equal(clipAcknowledgement.value.clipRevision, 7);
assert.equal(clipAcknowledgement.value.clipFile, '/clips/pass.mchq');
assert.equal(clipAcknowledgement.value.clipSamples, 12000000);
assert.equal(status({...live, receiverStartup: undefined,
  clipRevision: 7, clipError: 'Disk full\nclip stopped'}).value.clipError,
  'Disk full clip stopped');
for (const invalid of [0, -1, 1.5, '1'])
  assert.equal(status({...live, receiverStartup: undefined, clipRevision: invalid}).valid, false);
assert.equal(status({...live, receiverStartup: undefined, clipError: 'failed'}).valid, false);
assert.equal(status({...live, receiverStartup: undefined,
  clipFile: '/clips/pass.mchq'}).valid, false);
assert.equal(status({...live, receiverStartup: undefined,
  clipSamples: 1}).valid, false);
assert.equal(status({...replay, clipRevision: 1}).valid, false);
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
  {...startup, sdk: {...startup.sdk, parameters: {...startup.sdk.parameters, adcSampleRateHz: 0}}},
  {...startup, sdk: {...startup.sdk, parameters: {...startup.sdk.parameters, descriptorSampleRateHz: Infinity}}},
  {...startup, requested: {...startup.requested, lnaState: 9, frequency: 1000000000}},
  {...startup, selected: null}
]) assert.equal(status({...live, receiverStartup}).valid, false);
assert.equal(status({...replay, receiverStartup: startup}).valid, false);
const idleTuner = {active: false, detectedCount: 0, correctedCount: 0,
  lastDetectedAt: 0, lastCorrectedAt: 0};
const health = {schema: 1, receiver: 'RspDuo', monitoring: false,
  overload: {A: {...idleTuner}, B: {...idleTuner}}, ackFailures: 0};
const healthyLive = {...live, receiverHealth: health};
assert.deepEqual(status(healthyLive).value.receiverHealth, health);
assert.equal(Object.hasOwn(status(live).value, 'receiverHealth'), false,
  'Older processors must omit health rather than invent a healthy reading');
const onset = {...idleTuner, active: true, detectedCount: 1, lastDetectedAt: 1720000000000};
const corrected = {...onset, active: false, correctedCount: 1, lastCorrectedAt: 1720000001000};
const activeHealth = {...health, monitoring: true, overload: {A: onset, B: corrected}, ackFailures: 2};
assert.deepEqual(status({...live, receiverHealth: activeHealth}).value.receiverHealth, activeHealth);
const idleGain = requestedGainReductionDb => ({requestedGainReductionDb,
  requestedLnaState: 3, eventCount: 0, lastEventAt: 0, epoch: 0,
  metadataAvailable: false, reported: null});
const gainHealth = {...health, gain: {policy: 'epoch-on-vendor-change',
  sampleAligned: false, epoch: 2, tuners: {
    A: {...idleGain(30), eventCount: 1, lastEventAt: 1720000002000, epoch: 1,
      metadataAvailable: true, reported: {gRdB: 37, lnaGRdB: 12, currGain: 23.5}},
    B: {...idleGain(31), eventCount: 1, lastEventAt: 1720000003000, epoch: 2,
      metadataAvailable: true, reported: {gRdB: 41, lnaGRdB: 15, currGain: 19.25}}
  }}, sampleTime: {source: 'sdrplay-delivered-relative-sample-epoch',
    exact: false, uncertaintyBounded: false, sampleAligned: false,
    available: true, epoch: '3', nextSample: '4096'}};
assert.deepEqual(status({...live, receiverHealth: gainHealth}).value.receiverHealth,
  gainHealth, 'Per-tuner vendor gain events and capture epochs survive validation');
for (const invalidGain of [
  null, [], {...gainHealth.gain, policy: 'ignore'},
  {...gainHealth.gain, sampleAligned: true},
  {...gainHealth.gain, tuners: {...gainHealth.gain.tuners,
    A: {...gainHealth.gain.tuners.A, requestedGainReductionDb: 31}}},
  {...gainHealth.gain, tuners: {...gainHealth.gain.tuners,
    A: {...gainHealth.gain.tuners.A, epoch: 3}}},
  {...gainHealth.gain, tuners: {...gainHealth.gain.tuners,
    A: {...gainHealth.gain.tuners.A, metadataAvailable: false}}},
  {...gainHealth.gain, tuners: {...gainHealth.gain.tuners,
    A: {...gainHealth.gain.tuners.A, reported: {...gainHealth.gain.tuners.A.reported,
      currGain: Infinity}}}}
]) assert.equal(status({...live, receiverHealth: {...health, gain: invalidGain}}).valid,
  false, JSON.stringify(invalidGain));
for (const sampleTime of [
  {...gainHealth.sampleTime, exact: true},
  {...gainHealth.sampleTime, source: 'wall-clock'},
  {...gainHealth.sampleTime, epoch: 3},
  {...gainHealth.sampleTime, available: false},
  {...gainHealth.sampleTime, nextSample: '-1'}
]) assert.equal(status({...live,
  receiverHealth: {...gainHealth, sampleTime}}).valid, false, JSON.stringify(sampleTime));
for (const state of ['stopped', 'error']) {
  const stopped = {...live, state, error: state === 'error' ? 'Receiver disconnected' : '',
    receiverHealth: {...activeHealth, monitoring: false}};
  assert.equal(status(stopped).valid, true, 'Stopped receiver preserves the last observed overload episode');
}
assert.equal(status({...live, state: 'stopped', receiverHealth: activeHealth}).valid, false);
// Wall time can move backwards between actual transitions. Counts, not time
// ordering, establish the overload episode state.
assert.equal(status({...live, receiverHealth: {...health, overload: {
  A: {...corrected, lastCorrectedAt: corrected.lastDetectedAt - 1000}, B: idleTuner}}}).valid, true);
for (const receiverHealth of [
  null, [], false, 'healthy', {}, {...health, schema: 2}, {...health, receiver: 'Kraken'},
  {...health, monitoring: 1}, {...health, overload: []}, {...health, overload: {A: idleTuner}},
  {...health, overload: {A: idleTuner, B: null}}, {...health, ackFailures: undefined}
]) assert.equal(status({...live, receiverHealth}).valid, false, JSON.stringify(receiverHealth));
for (const field of ['detectedCount', 'correctedCount', 'lastDetectedAt', 'lastCorrectedAt']) {
  for (const invalid of [undefined, null, -1, 0.5, '0', NaN, Infinity, Number.MAX_SAFE_INTEGER + 1]) {
    assert.equal(status({...live, receiverHealth: {...health, overload: {
      A: {...idleTuner, [field]: invalid}, B: idleTuner}}}).valid, false, `${field}: ${invalid}`);
  }
}
for (const ackFailures of [-1, 0.5, '0', null, NaN, Infinity, Number.MAX_SAFE_INTEGER + 1])
  assert.equal(status({...live, receiverHealth: {...health, ackFailures}}).valid, false);
for (const A of [
  {...idleTuner, active: 0}, {...idleTuner, active: true},
  {...onset, active: false}, {...onset, detectedCount: 2},
  {...corrected, correctedCount: 2}, {...corrected, active: true},
  {...onset, lastDetectedAt: 0}, {...corrected, lastCorrectedAt: 0},
  {...idleTuner, lastDetectedAt: 1720000000000}, {...idleTuner, lastCorrectedAt: 1720000000000}
]) assert.equal(status({...live, receiverHealth: {...health, overload: {A, B: idleTuner}}}).valid, false);
const maximum = {...corrected, detectedCount: Number.MAX_SAFE_INTEGER, correctedCount: Number.MAX_SAFE_INTEGER};
assert.equal(status({...live, receiverHealth: {...health, ackFailures: Number.MAX_SAFE_INTEGER,
  overload: {A: maximum, B: idleTuner}}}).valid, true);
for (const outer of [replay, {...live, receiver: 'Kraken', receiverStartup: undefined}])
  assert.equal(status({...outer, receiverHealth: health}).valid, false);
const untrusted = {...health, message: '<img src=x onerror=alert(1)>',
  overload: {A: {...idleTuner, message: 'injected'}, B: {...idleTuner}, C: {...idleTuner}}};
const sanitized = status({...live, receiverHealth: untrusted}).value.receiverHealth;
assert.deepEqual(sanitized, health, 'Only allowlisted receiver health fields reach browser status');
assert.notEqual(sanitized, untrusted);
assert.notEqual(sanitized.overload.A, untrusted.overload.A);
assert.equal(fresh(null, 6000), false);
assert.equal(fresh({receivedAt: NaN}, 6000), false);
assert.equal(fresh({receivedAt: 1000}, 6000), true);
assert.equal(fresh({receivedAt: 1000}, 6001), false);

const usrpStartup = {schema: 1, receiver: 'Usrp', status: 'accepted',
  requested: {address: 'type=b200', subdevices: 'A:A A:B',
    frequency: 527000000, sampleRate: 2000000,
    antennas: ['RX2', 'RX2'], gains: [10, 20]},
  coherence: {motherboards: 1, channels: 2, motherboard: 'B210',
    clockSource: 'internal', timeSource: 'internal', subdevices: 'A:A A:B',
    policy: 'single-motherboard-b210-v1', singleMotherboard: true}};
const usrpHealth = {schema: 1, receiver: 'Usrp', monitoring: true,
  deviceTime: {source: 'uhd-device-time', utcMapped: false, continuous: true,
    sampleRateHz: 2000000, epochTick: '40000000', nextTick: '40000014',
    samples: '14', blocks: '2'}};
const usrpLive = {...replay, input: 'live', receiver: 'Usrp', state: 'live',
  file: '', frequency: 527000000, sampleRate: 2000000,
  receiverStartup: usrpStartup, receiverHealth: usrpHealth};
assert.deepEqual(status(usrpLive).value.receiverStartup, usrpStartup);
assert.deepEqual(status(usrpLive).value.receiverHealth, usrpHealth);
const usrpRecovery = {state: 'qualified', qualificationRequiredBlocks: 2,
  qualificationObservedBlocks: 2, discontinuities: '1', requalifications: '1',
  consecutiveDiscontinuities: '0', discardedBlocks: '2',
  discardedSamplesPerChannel: '21', lastDiscontinuity: 'UHD overflow'};
const recoveredUsrpHealth = {...usrpHealth,
  deviceTime: {...usrpHealth.deviceTime, epoch: '1'}, recovery: usrpRecovery};
assert.deepEqual(status({...usrpLive, receiverHealth: recoveredUsrpHealth})
  .value.receiverHealth, recoveredUsrpHealth);
const requalifyingUsrpHealth = {...recoveredUsrpHealth,
  deviceTime: {...recoveredUsrpHealth.deviceTime, continuous: false,
    epochTick: null, nextTick: null, samples: '0', blocks: '0'},
  recovery: {...usrpRecovery, state: 'requalifying',
    qualificationObservedBlocks: 1, consecutiveDiscontinuities: '1',
    requalifications: '0'}};
assert.equal(status({...usrpLive, receiverHealth: requalifyingUsrpHealth}).valid, true);
const unavailableUsrpHealth = {...requalifyingUsrpHealth,
  deviceTime: {...requalifyingUsrpHealth.deviceTime, epoch: '0'},
  recovery: {...requalifyingUsrpHealth.recovery, state: 'unavailable',
    qualificationObservedBlocks: 0, discontinuities: '0',
    consecutiveDiscontinuities: '0', discardedBlocks: '0',
    discardedSamplesPerChannel: '0', lastDiscontinuity: ''}};
assert.equal(status({...usrpLive, receiverHealth: unavailableUsrpHealth}).valid, true);
for (const recovery of [
  {...usrpRecovery, state: 'qualified', qualificationObservedBlocks: 1},
  {...usrpRecovery, qualificationRequiredBlocks: 3},
  {...usrpRecovery, requalifications: '2'},
  {...usrpRecovery, state: 'requalifying'}
]) assert.equal(status({...usrpLive,
  receiverHealth: {...recoveredUsrpHealth, recovery}}).valid, false,
  JSON.stringify(recovery));
assert.equal(status({...usrpLive, receiverStartup: {...usrpStartup, status: 'pending',
  coherence: null}, receiverHealth: {...usrpHealth, monitoring: false,
    deviceTime: {...usrpHealth.deviceTime, continuous: false, epochTick: null,
      nextTick: null, samples: '0', blocks: '0'}}}).valid, true);
for (const receiverStartup of [
  {...usrpStartup, coherence: {...usrpStartup.coherence, motherboards: 2}},
  {...usrpStartup, coherence: {...usrpStartup.coherence, clockSource: ''}},
  {...usrpStartup, coherence: {...usrpStartup.coherence, timeSource: ''}},
  {...usrpStartup, coherence: {...usrpStartup.coherence, motherboard: 'X310'}},
  {...usrpStartup, coherence: {...usrpStartup.coherence, policy: 'all-usrps'}},
  {...usrpStartup, coherence: {...usrpStartup.coherence, subdevices: 'A:B A:A'}},
  {...usrpStartup, requested: {...usrpStartup.requested, sampleRate: 6000000}},
  {...usrpStartup, status: 'pending'}
]) assert.equal(status({...usrpLive, receiverStartup}).valid, false,
  JSON.stringify(receiverStartup));
for (const deviceTime of [
  {...usrpHealth.deviceTime, source: 'host-arrival-time'},
  {...usrpHealth.deviceTime, utcMapped: true},
  {...usrpHealth.deviceTime, sampleRateHz: 6000000},
  {...usrpHealth.deviceTime, nextTick: '40000015'},
  {...usrpHealth.deviceTime, epochTick: null},
  {...usrpHealth.deviceTime, samples: 14},
  {...usrpHealth.deviceTime, continuous: false},
  {...usrpHealth.deviceTime, continuous: false, epochTick: null, nextTick: null,
    samples: '0', blocks: '1'}
]) assert.equal(status({...usrpLive,
  receiverHealth: {...usrpHealth, deviceTime}}).valid, false, JSON.stringify(deviceTime));
const sanitizedUsrp = status({...usrpLive,
  receiverStartup: {...usrpStartup, injected: '<script>'},
  receiverHealth: {...usrpHealth, injected: '<script>',
    deviceTime: {...usrpHealth.deviceTime, injected: '<script>'}}});
assert.equal(sanitizedUsrp.valid, true);
assert.equal(Object.hasOwn(sanitizedUsrp.value.receiverStartup, 'injected'), false);
assert.equal(Object.hasOwn(sanitizedUsrp.value.receiverHealth.deviceTime, 'injected'), false);

const hackrfStartup = {schema: 1, receiver: 'HackRF', status: 'accepted',
  requested: {frequency: 527000000, sampleRate: 6000000,
    serials: ['a1', 'b2'], lnaGainDb: [16, 32], vgaGainDb: [20, 40],
    ampEnabled: [false, true]},
  synchronization: {policy: 'shared-clock-hardware-trigger-v1',
    clockOutputRole: 'surveillance', hardwareTriggerRole: 'surveillance',
    startOrder: 'surveillance-then-reference', physicalWiringVerified: false,
    hardwareTimestampAvailable: false}};
const hackrfHealth = {schema: 1, receiver: 'HackRF', monitoring: true,
  pairing: {policy: 'paired-callback-requalify-v2', state: 'qualified', qualified: true,
    epoch: '1', callbacks: ['2', '2'], samples: ['4', '4'],
    pairedCallbacks: '2', pairedSamples: '4', discontinuities: '0',
    requalifications: '0', consecutiveDiscontinuities: '0', retryBudget: '8',
    qualificationRequiredPairs: 2, qualificationObservedPairs: 2,
    drainRequired: false, drainSeen: [false, false], discardedSamples: ['0', '0'],
    reason: 'paired hardware-triggered callbacks are aligned'}};
const hackrfLive = {...replay, input: 'live', receiver: 'HackRF', state: 'live',
  file: '', frequency: 527000000, sampleRate: 6000000,
  receiverStartup: hackrfStartup, receiverHealth: hackrfHealth};
assert.deepEqual(status(hackrfLive).value.receiverStartup, hackrfStartup);
assert.deepEqual(status(hackrfLive).value.receiverHealth, hackrfHealth);
assert.equal(status({...hackrfLive, receiverHealth: {...hackrfHealth, pairing: {
  ...hackrfHealth.pairing, callbacks: ['3', '2'], samples: ['6', '4']}}}).valid,
true, 'One staged callback is a valid transient and cannot publish alone');
const hackrfFault = {...hackrfHealth, monitoring: false, pairing: {
  ...hackrfHealth.pairing, state: 'unavailable', qualified: false, epoch: '10', callbacks: ['11', '2'],
  samples: ['20', '4'], discontinuities: '9', discardedSamples: ['20', '2'],
  consecutiveDiscontinuities: '9', qualificationObservedPairs: 0,
  reason: 'HackRF callback progression diverged'}};
assert.equal(status({...hackrfLive, receiverHealth: hackrfFault}).valid, true);
const hackrfRecovering = {...hackrfHealth, pairing: {
  ...hackrfHealth.pairing, state: 'requalifying', qualified: false, epoch: '2',
  callbacks: ['5', '4'], samples: ['10', '8'], discontinuities: '1',
  requalifications: '0', consecutiveDiscontinuities: '1',
  qualificationObservedPairs: 1, drainRequired: false, drainSeen: [true, true],
  discardedSamples: ['4', '4'], reason: 'awaiting second matched callback pair'}};
assert.equal(status({...hackrfLive, receiverHealth: hackrfRecovering}).valid, true);
for (const receiverStartup of [
  {...hackrfStartup, requested: {...hackrfStartup.requested, serials: ['a1', 'a1']}},
  {...hackrfStartup, requested: {...hackrfStartup.requested, lnaGainDb: [15, 32]}},
  {...hackrfStartup, synchronization: {...hackrfStartup.synchronization,
    physicalWiringVerified: true}},
  {...hackrfStartup, status: 'pending'}
]) assert.equal(status({...hackrfLive, receiverStartup}).valid, false,
  JSON.stringify(receiverStartup));
for (const pairing of [
  {...hackrfHealth.pairing, policy: 'best-effort'},
  {...hackrfHealth.pairing, state: 'requalifying'},
  {...hackrfHealth.pairing, retryBudget: '9'},
  {...hackrfHealth.pairing, qualificationRequiredPairs: 1},
  {...hackrfHealth.pairing, drainRequired: true},
  {...hackrfHealth.pairing, requalifications: '1'},
  {...hackrfHealth.pairing, epoch: 1}
]) assert.equal(status({...hackrfLive,
  receiverHealth: {...hackrfHealth, pairing}}).valid, false, JSON.stringify(pairing));
const sanitizedHackrf = status({...hackrfLive,
  receiverStartup: {...hackrfStartup, injected: '<script>'},
  receiverHealth: {...hackrfHealth, injected: '<script>',
    pairing: {...hackrfHealth.pairing, injected: '<script>'}}});
assert.equal(sanitizedHackrf.valid, true);
assert.equal(Object.hasOwn(sanitizedHackrf.value.receiverHealth.pairing, 'injected'), false);
console.log('Processor status validation and freshness tests passed.');
