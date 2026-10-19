'use strict';

const assert = require('node:assert/strict');
const {test} = require('node:test');
const {createGainMonitor} = require('./gain-monitor');
const {LIMITS} = require('./automatic-gain');

function config() {
  return {capture: {fc: 527000000, fs: 2400000, replay: {state: false},
    device: {type: 'Kraken', channel_count: 2,
      heimdall: {host: '127.0.0.1', port: 8091, gain: 14, gain_control: 'measured'}}}};
}
function probe(c, safe = false) {
  return {frequencyHz: c.capture.fc, sampleRateHz: c.capture.fs,
    channelCount: c.capture.device.channel_count, gainDb: c.capture.device.heimdall.gain,
    channels: Array.from({length: c.capture.device.channel_count}, (_, channel) => ({channel,
      samples: 600000, railFraction: safe ? 0 : .001, peakFraction: safe ? .8 : 1,
      p999Fraction: .6, rmsFraction: .2}))};
}
function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return {promise, resolve, reject};
}
function harness(options = {}) {
  let c = config(), revision = 'revision-1', busy = false, nextId = 1;
  const timers = new Map(), reports = [], calls = [], overloads = [];
  const monitor = createGainMonitor({getConfig: () => c, getRevision: () => revision, busy: () => busy,
    measureGain: (snapshot, captureOptions) => {
      calls.push({config: snapshot, options: captureOptions});
      return options.measureGain ? options.measureGain(snapshot, captureOptions, calls.length) : probe(snapshot);
    },
    onOverload: value => { overloads.push(value); return options.onOverload?.(value); },
    report: value => { reports.push(value); options.report?.(value); },
    setTimeoutImpl: (fn, delay) => { const id = nextId++; timers.set(id, {fn, delay}); return id; },
    clearTimeoutImpl: id => timers.delete(id)});
  return {monitor, timers, calls, reports, overloads, get config() { return c; },
    set config(value) { c = value; }, set revision(value) { revision = value; },
    set busy(value) { busy = value; },
    fire() {
      assert.equal(timers.size, 1, 'exactly one timer must be pending');
      const [id, timer] = timers.entries().next().value; timers.delete(id);
      assert.equal(timer.delay, 60000);
      return timer.fn();
    }};
}

test('two fresh unsafe probes trigger once, with a snapshot and shared bounded capture settings', async () => {
  const h = harness(); h.monitor.start(); h.monitor.start();
  assert.equal(h.calls.length, 0, 'start waits one minute');
  assert.equal(h.timers.size, 1);
  await h.fire();
  assert.equal(h.monitor.status().consecutiveUnsafe, 1); assert.equal(h.overloads.length, 0);
  await h.fire();
  assert.equal(h.calls.length, 2); assert.equal(h.overloads.length, 1);
  assert.equal(h.overloads[0].revision, 'revision-1');
  assert.equal(h.overloads[0].probes.length, 2);
  assert.notEqual(h.overloads[0].config, h.config);
  assert.deepEqual(h.overloads[0].config, h.config);
  for (const call of h.calls) {
    assert.equal(call.options.captureSeconds, LIMITS.probeSeconds);
    assert.equal(call.options.timeoutMs, LIMITS.probeTimeoutMs);
    assert.ok(call.options.signal instanceof AbortSignal);
  }
  assert.equal(h.monitor.status().triggered, true); assert.equal(h.timers.size, 0);
  h.monitor.stop(); h.monitor.start();
  assert.equal(h.timers.size, 0, 'the overload latch cannot be restarted');
});

test('a safe probe clears the unsafe streak', async () => {
  const h = harness({measureGain: (c, _, n) => probe(c, n === 2)}); h.monitor.start();
  await h.fire(); await h.fire(); assert.equal(h.monitor.status().consecutiveUnsafe, 0);
  await h.fire(); assert.equal(h.overloads.length, 0);
  await h.fire(); assert.equal(h.overloads.length, 1);
});

test('revision or capture tuple changes prevent combining unrelated unsafe probes', async () => {
  for (const change of [h => { h.revision = 'revision-2'; },
    h => { h.config.capture.device.heimdall.gain = 13; }]) {
    const h = harness(); h.monitor.start(); await h.fire(); change(h);
    await h.fire(); assert.equal(h.overloads.length, 0);
    assert.equal(h.monitor.status().consecutiveUnsafe, 1);
    h.monitor.stop();
  }
});

test('in-flight revision changes discard results and no probe overlaps', async () => {
  const pending = deferred(), h = harness({measureGain: () => pending.promise}); h.monitor.start();
  const run = h.fire();
  assert.equal(h.monitor.status().active, true); assert.equal(h.timers.size, 0);
  h.monitor.start(); assert.equal(h.calls.length, 1); assert.equal(h.timers.size, 0);
  h.revision = 'revision-2'; pending.resolve(probe(h.config)); await run;
  assert.equal(h.monitor.status().consecutiveUnsafe, 0); assert.equal(h.overloads.length, 0);
  assert.equal(h.reports.at(-1).reason, 'configuration-changed'); h.monitor.stop();
});

test('busy skips and resets the streak, and busy after capture discards its result', async () => {
  const h = harness(); h.monitor.start(); await h.fire(); h.busy = true;
  await h.fire(); assert.equal(h.calls.length, 1); assert.equal(h.monitor.status().consecutiveUnsafe, 0);
  h.busy = false; await h.fire(); assert.equal(h.overloads.length, 0); h.monitor.stop();
  const pending = deferred(), other = harness({measureGain: () => pending.promise}); other.monitor.start();
  const run = other.fire(); other.busy = true; pending.resolve(probe(other.config)); await run;
  assert.equal(other.monitor.status().consecutiveUnsafe, 0); assert.equal(other.reports.at(-1).reason, 'busy');
  other.monitor.stop();
});

test('rechecks busy and revision after reporting the second unsafe result', async () => {
  for (const mutate of [h => { h.busy = true; }, h => { h.revision = 'revision-2'; }]) {
    let h;
    h = harness({report: event => { if (event.status === 'unsafe' && event.monitor.consecutiveUnsafe === 2) mutate(h); }});
    h.monitor.start(); await h.fire(); await h.fire();
    assert.equal(h.overloads.length, 0); assert.equal(h.monitor.status().consecutiveUnsafe, 0); h.monitor.stop();
  }
});

test('probe, calibration and malformed-result errors report and reset without tuning', async () => {
  for (const failure of [() => { const error = new Error('retuning'); error.code = 'KRAKEN_GAIN_PROBE_UNCALIBRATED'; throw error; },
    () => { const error = new Error('timed out'); error.code = 'KRAKEN_GAIN_PROBE_TIMEOUT'; throw error; },
    () => ({}), c => ({...probe(c), gainDb: 1})]) {
    const h = harness({measureGain: (c, _, n) => n === 2 ? failure(c) : probe(c)});
    h.monitor.start(); await h.fire(); await h.fire();
    assert.equal(h.monitor.status().consecutiveUnsafe, 0); assert.equal(h.reports.at(-1).status, 'error');
    assert.equal(h.reports.at(-1).phase, 'probe'); await h.fire();
    assert.equal(h.overloads.length, 0); h.monitor.stop();
  }
});

test('fixed mode, replay, other receivers and unknown/nonfixed gain never probe', async () => {
  for (const mutate of [c => { c.capture.device.heimdall.gain_control = 'fixed'; },
    c => { delete c.capture.device.heimdall.gain_control; }, c => { c.capture.replay.state = true; },
    c => { c.capture.device.type = 'RspDuo'; }, c => { c.capture.device.heimdall.gain = 'keep'; },
    c => { c.capture.device.heimdall.gain = -1; }]) {
    const h = harness(); mutate(h.config); h.monitor.start(); await h.fire();
    assert.equal(h.calls.length, 0); assert.equal(h.reports.at(-1).reason, 'disabled'); h.monitor.stop();
  }
});

test('stop clears timers, aborts active capture and discards even a late successful result', async () => {
  const pending = deferred(), h = harness({measureGain: () => pending.promise}); h.monitor.start();
  const run = h.fire(); h.monitor.stop();
  assert.equal(h.calls[0].options.signal.aborted, true); assert.equal(h.timers.size, 0);
  h.monitor.start(); // Even a misbehaving dependency ignoring abort cannot revive its old result.
  pending.resolve(probe(h.config)); await run;
  assert.equal(h.monitor.status().consecutiveUnsafe, 0); assert.equal(h.overloads.length, 0);
  h.monitor.stop(); assert.equal(h.timers.size, 0);
});

test('overload callback failure remains latched and cannot cause retry storms', async () => {
  const h = harness({onOverload: () => { throw new Error('apply failed'); }}); h.monitor.start();
  await h.fire(); await h.fire();
  assert.equal(h.overloads.length, 1); assert.equal(h.monitor.status().triggered, true);
  assert.equal(h.reports.at(-1).phase, 'onOverload'); assert.equal(h.timers.size, 0);
  h.monitor.start(); assert.equal(h.timers.size, 0);
});

test('unknown configuration revision is an error, never an implicit authorization', async () => {
  const h = harness(); h.revision = undefined; h.monitor.start(); await h.fire();
  assert.equal(h.calls.length, 0); assert.equal(h.reports.at(-1).status, 'error'); h.monitor.stop();
});
