'use strict';
const assert = require('assert');
const {createGpuSetupStatus, fromRuntime, isPi} = require('./gpu-setup');

const older = {version: 1, pi: true, state: 'compiler-risk', qualification: 'not-run',
  message: 'Possible missing backport', serviceAccess: {state: 'available'}};
const ready = {active: 'vulkan', state: 'ready'};
const fallback = {active: 'cpu', state: 'fallback', reason: 'Startup mismatch'};
const runtime = {acceleration: ready, clutterAcceleration: ready, fresh: true};

(async () => {
  assert(isPi(() => 'Raspberry Pi 4 Model B Rev 1.5\0'));
  assert(!isPi(() => 'Generic aarch64 workstation'));
  assert(!isPi(() => { throw new Error('no device-tree'); }));
  assert.equal(fromRuntime(older, {...runtime, fresh: false}).state, 'compiler-risk');
  assert.equal(fromRuntime(older, {...runtime, acceleration: fallback, clutterAcceleration: fallback}).state, 'compiler-risk');
  assert.equal(fromRuntime(older, {...runtime, clutterAcceleration: fallback}).state, 'partially-qualified');
  assert.equal(fromRuntime(older, {...runtime, acceleration: fallback}).state, 'partially-qualified');
  assert.equal(fromRuntime(older, runtime).state, 'qualified', 'A working backport overrides a source-version hint');
  assert.equal(fromRuntime(older, runtime).qualification, 'current-telemetry-generation');
  assert.match(fromRuntime(older, runtime).message, /^Current GPU checks passed: ambiguity and clutter\.$/);
  let calls = 0, at = 0;
  const execute = (program, args, options, callback) => {
    calls++;
    assert.equal(program, '/usr/bin/python3');
    assert.deepEqual(args.slice(-2), ['--status', '--json']);
    assert(!args.includes('--install-driver'));
    assert.equal(options.timeout, 8000); assert.equal(options.maxBuffer, 65536);
    assert(!options.shell && !options.env.LD_PRELOAD && !options.env.VK_DRIVER_FILES);
    callback(null, JSON.stringify(older));
  };
  const get = createGpuSetupStatus({pi: true, execute, now: () => at});
  await Promise.all([get(runtime), get(runtime)]); assert.equal(calls, 1);
  assert.equal((await get({...runtime, fresh: false})).state, 'compiler-risk', 'Cached enumeration never caches live acceptance');
  at = 60001; await get(runtime); assert.equal(calls, 2);
  const desktop = createGpuSetupStatus({pi: false, execute: (_p, _a, _o, callback) => {
    calls++;
    callback(null, JSON.stringify({...older, pi: false, state: 'service-access-needed',
      serviceAccess: {state: 'group-access-needed'}}));
  }});
  assert.equal((await desktop({fresh: false})).state, 'service-access-needed'); assert.equal(calls, 3);
  const brokerMissing = createGpuSetupStatus({pi: false, execute: (_p, _a, _o, callback) => {
    callback(null, JSON.stringify({...older, pi: false, state: 'driver-unverified',
      serviceAccess: {state: 'unavailable'}, message: 'Activate the updated receiver helper'}));
  }});
  assert.equal((await brokerMissing(runtime)).serviceAccess.state, 'unavailable');
  const preview = createGpuSetupStatus({pi: true, preview: true, execute});
  assert.equal((await preview(runtime)).qualification, 'not-run'); assert.equal(calls, 3);
  let completeDelayedSetup;
  let delayedFresh = true;
  const delayed = createGpuSetupStatus({pi: true, execute: (_p, _a, _o, callback) => {
    completeDelayedSetup = () => callback(null, JSON.stringify(older));
  }});
  const pending = delayed(() => ({...runtime, fresh: delayedFresh}));
  delayedFresh = false; // A disconnect/restart occurred during the bounded helper query.
  completeDelayedSetup();
  assert.equal((await pending).state, 'compiler-risk',
    'Freshness is read after the helper returns, never before its bounded wait');
  for (const output of ['garbage', JSON.stringify({...older, state: 'qualified'}),
    JSON.stringify({...older, qualification: 'passed'}), JSON.stringify({...older, message: 'x'.repeat(2049)})]) {
    const invalid = createGpuSetupStatus({pi: true, execute: (_p, _a, _o, cb) => cb(null, output)});
    assert.equal((await invalid({fresh: false})).state, 'driver-unverified');
  }
  const failure = createGpuSetupStatus({pi: true, execute: (_p, _a, _o, cb) => cb(new Error('timeout'))});
  assert.equal((await failure({fresh: false})).qualification, 'not-run');
  console.log('GPU setup API: detection, caching, bounded failure, stale telemetry, backport and per-stage qualification PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
