'use strict';

const assert = require('assert/strict');
const {createAutomaticGain, enabled, LIMITS, MAX_AUTOMATIC_GAIN_MS} = require('./automatic-gain');
const clone = value => JSON.parse(JSON.stringify(value));
const config = (gain = 20) => ({capture: {fc: 527000000, fs: 2400000,
  replay: {state: false}, device: {type: 'Kraken', channel_count: 5,
    heimdall: {host: 'localhost', port: 8091, control_port: 8092, gain, gain_control: 'measured'}}}});
function status(config, gain = config.capture.device.heimdall.gain) {
  return {centerFrequency: config.capture.fc, sampleRate: config.capture.fs,
    channelCount: config.capture.device.channel_count, gain, operatingMode: 'coherent',
    reconfiguring: false, recovering: false, cooldownActive: false};
}
function measurement(config, p999 = .6, overrides = {}) {
  return {frequencyHz: config.capture.fc, sampleRateHz: config.capture.fs,
    channelCount: config.capture.device.channel_count, gainDb: config.capture.device.heimdall.gain,
    channels: Array.from({length: config.capture.device.channel_count}, (_, channel) => ({
      channel, samples: 600000, railFraction: 0, peakFraction: Math.max(.6, p999),
      p999Fraction: p999, rmsFraction: p999 / 2})), ...overrides};
}
function fixture({previous = config(), probes = [.6, .6], actualGain = 20,
  syncError, calibrationError, verifyError, verifyDrift, syncBusy = false, clock} = {}) {
  const calls = []; let live = status(previous, actualGain);
  let syncs = 0, calibrations = 0, verifies = 0, captures = 0;
  function receipt(operations = []) { return {status: 'synchronized', before: clone(live),
    after: clone(live), operations}; }
  function injected(code) { const error = new Error(code); error.code = code; return error; }
  const synchronizer = {
    async verify(next) {
      calls.push({stage: 'verify', config: clone(next)}); verifies++;
      if (clock) clock.value += 75000;
      if (verifyError === verifies) throw injected('VERIFY_FAILED');
      if (verifyDrift === verifies) live.centerFrequency++;
      return receipt();
    },
    async synchronize(before, next, options) {
      assert.equal(options.force, true);
      calls.push({stage: 'synchronize', config: clone(next)}); syncs++;
      if (clock) clock.value += 75000;
      live = status(next);
      if (syncBusy) { live.recovering = true; live.cooldownActive = true; }
      const result = receipt([{operation: 'set_gain', commandSent: true,
        acknowledged: true, readbackMatched: true, requested: live.gain}]);
      if (syncError === syncs) {
        const error = injected('SYNC_TIMEOUT'); error.receiverSync = result; throw error;
      }
      return result;
    },
    async recalibrate(next) {
      calls.push({stage: 'recalibrate', config: clone(next)}); calibrations++;
      if (clock) clock.value += 150000;
      if (calibrationError === calibrations) throw injected('CALIBRATION_FAILED');
      live.recovering = false; live.cooldownActive = false;
      return {...receipt([{operation: 'reset_lag_compensation', commandSent: true,
        acknowledged: true, readbackMatched: true, fullRecalibration: {commandSent: true,
          acknowledged: true, recoveryObserved: true, completed: true,
          calibrationState: 'CONVERGED', phaseState: 4, noiseSource: false}}]), status: 'recalibrated'};
    }
  };
  async function measureGain(next) {
    calls.push({stage: 'probe', config: clone(next)});
    if (clock) clock.value += 15000;
    const value = probes[captures++];
    assert.notEqual(value, undefined, 'Only the expected independent captures may run');
    if (value instanceof Error) throw value;
    return typeof value === 'function' ? value(next) : measurement(next, value);
  }
  return {controller: createAutomaticGain({synchronizer, measureGain,
    ...(clock ? {now: () => clock.value} : {})}), previous,
    candidate: clone(previous), calls, live: () => clone(live), synchronizer};
}
const gains = run => run.calls.filter(call => call.stage === 'synchronize')
  .map(call => call.config.capture.device.heimdall.gain);
async function rejected(run, code) {
  let error;
  try { await run.controller.apply(run.previous, run.candidate); }
  catch (caught) { error = caught; }
  assert.equal(error?.code, code); assert.ok(error.receiverSync); return error;
}

(async () => {
  assert.equal(enabled(config()), true);
  const replay = config(); replay.capture.replay.state = true; assert.equal(enabled(replay), false);
  const manual = config(); manual.capture.device.heimdall.gain_control = 'manual';
  assert.equal(enabled(manual), false);
  assert.equal(enabled({capture: {device: {type: 'RspDuo'}}}), false);

  // An unchanged gain still receives full recalibration and two new captures.
  let run = fixture();
  const originals = [clone(run.previous), clone(run.candidate)];
  let result = await run.controller.apply(run.previous, run.candidate);
  assert.deepEqual(run.calls.map(call => call.stage),
    ['verify', 'synchronize', 'recalibrate', 'probe', 'probe', 'verify']);
  assert.deepEqual([run.previous, run.candidate], originals);
  assert.equal(result.config.capture.device.heimdall.gain, 20);
  assert.equal(result.receiverSync.automaticGain.finalPassingProbes, 2);
  assert.equal(result.receiverSync.automaticGain.status, 'verified');
  assert.equal(result.receiverSync.automaticGain.claims.rawAdcClippingProven, false);
  assert.equal(result.receiverSync.automaticGain.claims.radarQualificationProven, false);
  assert.deepEqual(result.receiverSync.operations.map(operation => operation.automaticGainStage),
    ['trial-synchronize', 'trial-recalibrate']);

  // Synchronization may return a matching tuple while Suite recovery/cooldown
  // is active. Full recalibration owns waiting for idle before its reset.
  run = fixture({syncBusy: true});
  result = await run.controller.apply(run.previous, run.candidate);
  await result.rollback();
  assert.equal(result.receiverSync.automaticGain.rollback.status, 'restored');
  assert.deepEqual(run.calls.map(call => call.stage), ['verify', 'synchronize',
    'recalibrate', 'probe', 'probe', 'verify', 'synchronize', 'recalibrate', 'verify']);

  run = fixture({verifyDrift: 2});
  const finalDrift = await rejected(run, 'AUTOMATIC_GAIN_RECEIVER_MISMATCH');
  assert.equal(finalDrift.receiverSync.automaticGain.rollback.status, 'restored');
  assert.equal(finalDrift.receiverSync.automaticGain.selectedGainDb, null);
  assert.deepEqual(run.live(), status(run.previous));
  assert.deepEqual(run.calls.map(call => call.stage), ['verify', 'synchronize',
    'recalibrate', 'probe', 'probe', 'verify', 'synchronize', 'recalibrate', 'verify']);

  // Persistence failure restores the actual old numeric gain, even for keep,
  // with one idempotent restoration regardless of repeated/concurrent calls.
  run = fixture({previous: config('keep'), actualGain: 23.4});
  run.candidate.capture.fc += 1000000;
  run.candidate.capture.device.heimdall.gain = 30;
  result = await run.controller.apply(run.previous, run.candidate);
  const [restored, again] = await Promise.all([result.rollback(), result.rollback()]);
  assert.equal(restored, again);
  assert.equal(restored.automaticGain.rollback.status, 'restored');
  assert.deepEqual(run.live(), status(run.previous, 23.4));
  assert.deepEqual(gains(run), [30, 23.4]);
  assert.equal(run.calls.filter(call => call.stage === 'recalibrate').length, 2);
  assert.equal(restored.automaticGain.selectedGainDb, null);

  run = fixture({previous: config(14), actualGain: 14, probes: [next => {
    const probe = measurement(next, 1); probe.channels[4].railFraction = .001; return probe;
  }, .5, .5]});
  result = await run.controller.apply(run.previous, run.candidate);
  assert.deepEqual(gains(run), [14, 13]);
  assert.equal(result.receiverSync.automaticGain.selectedGainDb, 13,
    'The smallest common Suite request is tested before reducing further');
  assert.equal(result.receiverSync.automaticGain.gainSteps.hardwareGainStepsVerified, false);
  assert.equal(result.receiverSync.automaticGain.gainSteps.commonRequestedGainStepDb, 1);
  const clipped = next => {
    const probe = measurement(next, 1); probe.channels[4].railFraction = .001; return probe;
  };
  run = fixture({previous: config(14), actualGain: 14,
    probes: Array.from({length: 15}, () => clipped)});
  let clippedError = await rejected(run, 'AUTOMATIC_GAIN_UNSAFE_AT_MINIMUM');
  assert.deepEqual(gains(run), [...Array.from({length: 15}, (_, index) => 14 - index), 14]);
  assert.equal(clippedError.receiverSync.automaticGain.rollback.status, 'restored');

  // Worst channel controls the down-step; strong average power cannot hide it.
  run = fixture({probes: [next => {
    const probe = measurement(next, .3); Object.assign(probe.channels[4],
      {p999Fraction: .9, peakFraction: .96}); return probe;
  }, .5, .5]});
  await run.controller.apply(run.previous, run.candidate);
  assert.deepEqual(gains(run), [20, 19]);

  // Weak samples permit only one requested dB at a time. An unsafe neighbor
  // restores the last safe gain, and fallback needs two fresh passing probes.
  run = fixture({probes: [.2, .8, .2, .2]});
  result = await run.controller.apply(run.previous, run.candidate);
  assert.deepEqual(gains(run), [20, 21, 20]);
  assert.equal(result.receiverSync.automaticGain.probes.length, 4);
  assert.equal(result.receiverSync.automaticGain.selectedGainDb, 20);
  run = fixture({probes: [.2, .6, .8, .2, .2]});
  await run.controller.apply(run.previous, run.candidate);
  assert.deepEqual(gains(run), [20, 21, 20], 'A failed confirming probe also falls back');
  run = fixture({probes: [.2, .3, .4, .6, .6]});
  result = await run.controller.apply(run.previous, run.candidate);
  assert.deepEqual(gains(run), [20, 21, 22, 23]);
  assert.equal(result.receiverSync.automaticGain.selectionReason, 'headroom-target');
  run = fixture({probes: [.2, .3, .8, .3, .3]});
  result = await run.controller.apply(run.previous, run.candidate);
  assert.deepEqual(gains(run), [20, 21, 22, 21]);
  assert.equal(result.receiverSync.automaticGain.selectedGainDb, 21);
  run = fixture({probes: [.2, .2]});
  result = await run.controller.apply(run.previous, run.candidate, {increase: false});
  assert.deepEqual(gains(run), [20]);
  assert.equal(result.receiverSync.automaticGain.selectionReason, 'backoff-only');
  run = fixture({probes: [.9, .8, .5, .5]});
  await run.controller.apply(run.previous, run.candidate);
  assert.deepEqual(gains(run), [20, 19, 18], 'A downward search stops at the first verified safe setting');

  for (const field of ['frequencyHz', 'sampleRateHz', 'channelCount', 'gainDb']) {
    run = fixture({probes: [next => {
      const probe = measurement(next); probe[field]++; return probe;
    }]});
    const error = await rejected(run, 'AUTOMATIC_GAIN_PROBE_MISMATCH');
    assert.equal(error.receiverSync.automaticGain.status, 'failed-restored');
    assert.equal(error.receiverSync.automaticGain.rollback.status, 'restored');
    assert.deepEqual(gains(run), [20, 20]);
  }
  for (const mutate of [probe => probe.channels.pop(),
    probe => { probe.channels[4].channel = 0; },
    probe => { probe.channels[4].samples = 0; },
    probe => { probe.channels[4].peakFraction = NaN; },
    probe => { probe.channels[4].railFraction = -1; },
    probe => { probe.channels[4].rmsFraction = Infinity; }]) {
    run = fixture({probes: [next => { const probe = measurement(next); mutate(probe); return probe; }]});
    const error = await rejected(run, mutate.toString().includes('pop') ?
      'AUTOMATIC_GAIN_PROBE_MISMATCH' : 'AUTOMATIC_GAIN_INVALID_PROBE');
    assert.equal(error.receiverSync.automaticGain.rollback.status, 'restored');
  }

  run = fixture({actualGain: -1});
  await rejected(run, 'AUTOMATIC_GAIN_RECEIVER_MISMATCH');
  assert.deepEqual(run.calls.map(call => call.stage), ['verify']);
  run = fixture({actualGain: 19});
  await rejected(run, 'AUTOMATIC_GAIN_RECEIVER_MISMATCH');
  assert.deepEqual(gains(run), []);
  run = fixture(); run.candidate.capture.device.heimdall.host = 'other-host';
  await rejected(run, 'AUTOMATIC_GAIN_ENDPOINT_CHANGED'); assert.deepEqual(run.calls, []);
  run = fixture(); run.candidate.capture.device.heimdall.gain = -1;
  await rejected(run, 'AUTOMATIC_GAIN_INVALID_GAIN'); assert.deepEqual(run.calls, []);

  run = fixture({calibrationError: 1});
  let error = await rejected(run, 'CALIBRATION_FAILED');
  assert.equal(run.calls.filter(call => call.stage === 'probe').length, 0);
  assert.equal(error.receiverSync.automaticGain.rollback.status, 'restored');
  const timeout = new Error('Bounded probe timed out'); timeout.code = 'GAIN_PROBE_TIMEOUT';
  run = fixture({probes: [timeout]});
  error = await rejected(run, 'GAIN_PROBE_TIMEOUT');
  assert.equal(error.receiverSync.automaticGain.rollback.status, 'restored');
  run = fixture({syncError: 1});
  error = await rejected(run, 'SYNC_TIMEOUT');
  assert.equal(error.receiverSync.operations[0].commandSent, true);
  assert.equal(error.receiverSync.automaticGain.rollback.status, 'restored');
  run = fixture({probes: [timeout], calibrationError: 2});
  error = await rejected(run, 'GAIN_PROBE_TIMEOUT');
  assert.equal(error.receiverSync.status, 'indeterminate');
  assert.equal(error.receiverSync.automaticGain.rollback.status, 'failed');
  assert.equal(error.receiverSync.automaticGain.rollback.code, 'CALIBRATION_FAILED');
  assert.deepEqual(gains(run), [20, 20]);

  run = fixture({actualGain: 0, previous: config(0), probes: [.8]});
  error = await rejected(run, 'AUTOMATIC_GAIN_UNSAFE_AT_MINIMUM');
  assert.deepEqual(gains(run), [0, 0]);
  run = fixture({probes: Array(16).fill(.8)});
  error = await rejected(run, 'AUTOMATIC_GAIN_NO_SAFE_SETTING');
  assert.deepEqual(gains(run), [...Array.from({length: 16}, (_, index) => 20 - index), 20]);
  assert.equal(error.receiverSync.automaticGain.trials.length, 16);
  assert.equal(error.receiverSync.automaticGain.rollback.status, 'restored');

  // A still-clear measurement at the count bound must not be called a peak.
  run = fixture({probes: Array(16).fill(.2)});
  error = await rejected(run, 'AUTOMATIC_GAIN_NO_SAFE_SETTING');
  assert.equal(error.receiverSync.automaticGain.selectedGainDb, null);
  assert.equal(error.receiverSync.automaticGain.rollback.status, 'restored');
  assert.ok(error.receiverSync.automaticGain.trials.length <= LIMITS.maxTrialSettings);

  // Full stage reservations enforce fifteen minutes without racing live I/O;
  // rollback still gets its own complete five-minute budget.
  const clock = {value: 0};
  run = fixture({probes: Array(16).fill(.2), clock});
  error = await rejected(run, 'AUTOMATIC_GAIN_DEADLINE');
  assert.equal(error.receiverSync.automaticGain.rollback.status, 'restored');
  assert.equal(run.calls.filter(call => call.stage === 'probe').length, 3);
  assert.ok(clock.value <= MAX_AUTOMATIC_GAIN_MS);
  assert.equal(clock.value, 1170000);
  assert.equal(run.calls.at(-1).stage, 'verify');

  // A rejected post-save restore is itself idempotent and retains the receipt.
  run = fixture({syncError: 2}); result = await run.controller.apply(run.previous, run.candidate);
  await assert.rejects(result.rollback(), caught => caught.receiverSync.automaticGain.rollback.status === 'failed');
  await assert.rejects(result.rollback(), caught => caught.receiverSync.automaticGain.rollback.status === 'failed');
  assert.deepEqual(gains(run), [20, 20]);
  console.log('Measured common gain: bounded sequential calibration/probes, all-channel gates, fixed-gain restore and failure receipts passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
