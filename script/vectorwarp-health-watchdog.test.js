'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const {EventEmitter} = require('events');
const {receiverSettlement, requested, sameSettings, synchronizeAfterRestart, applicationInterlock,
  processorProgress, evidenceFailure, FRAME_RECOVERY_TIMEOUT_MS, RECEIVER_READINESS_TIMEOUT_MS} =
  require('./vectorwarp-health-watchdog');
const {evaluateFrameLiveness} = require('../api/frame-liveness');

const stat = '42 (worker (resident)) S 1 2 3 4 5 6 7 8 9 10 123 456 999 888 0 0 1 0 45678';
assert.equal(processorProgress(42, filename => {
  assert.equal(filename, '/proc/42/stat');
  return stat;
}), 579, 'count only exact parent user/system ticks, not child ticks');
assert.equal(processorProgress(43, () => stat), null, 'wrong process identity fails closed');
for (const value of [stat.replace('123', '-1'), stat.replace('456', '1e3'),
  stat.replace('123', '9007199254740992'), '42 malformed', '42 (worker) S'])
  assert.equal(processorProgress(42, () => value), null);
assert.equal(processorProgress(42, () => { throw new Error('process exited'); }), null);
assert.equal(processorProgress(0, () => { throw new Error('must not read'); }), null);
assert.equal(evidenceFailure({evidenceHealth: {status: 'healthy'}}), null);
assert.deepEqual(evidenceFailure({evidenceHealth: {status: 'degraded', channels: [
  {id: 'primary', errors: ['EACCES: evidence ledger is not writable']}]}}),
['EACCES: evidence ledger is not writable'],
'A persistence failure must prevent an otherwise live processor from reporting healthy');

// The installed oneshot must not truncate its own receiver/frame wait budget.
const unit = fs.readFileSync(path.join(__dirname,
  '../contrib/systemd/vectorwarp-health-watchdog.service.in'), 'utf8');
const unitTimeoutMs = Number(/^TimeoutStartSec=(\d+)$/m.exec(unit)?.[1]) * 1000;
assert(Number.isFinite(unitTimeoutMs));
assert(unitTimeoutMs >= FRAME_RECOVERY_TIMEOUT_MS + RECEIVER_READINESS_TIMEOUT_MS + 120000,
  'packaged watchdog allows frame/receiver deadlines plus bounded service-transition headroom');
assert(unitTimeoutMs <= 600000, 'the watchdog oneshot retains an explicit hard upper bound');

const config = {capture: {fc: 563000000, fs: 2400000, device: {type: 'Kraken',
  channel_count: 5, heimdall: {gain: 2, channel_gains: [19.7, 25.4, 20.7, 25.4, 22.9]}}}};
const wanted = requested(config);
assert.deepEqual(wanted, {frequency: 563000000, sampleRate: 2400000, channelCount: 5,
  gain: 2, channelGains: [19.7, 25.4, 20.7, 25.4, 22.9]});
assert(sameSettings(wanted, {...wanted}));
assert(!sameSettings(wanted, {...wanted, frequency: 527000000}));
assert(!sameSettings(wanted, {...wanted, channelGains: [19.7, 25.4, 20.7, 25.4, 21.9]}));
const receipt = {status: 'synchronized', requested: wanted, operations: []};
assert.deepEqual(receiverSettlement('revision-a', wanted, receipt, 42), {
  state: 'synchronized', completedAt: 42, receipt, configRevision: 'revision-a',
  applicationVerified: true, reconciliationRequired: false});
assert.throws(() => receiverSettlement('revision-b', wanted,
  {...receipt, requested: {...wanted, frequency: 527000000}}), /exact configuration/);
assert.throws(() => receiverSettlement('revision-b', wanted,
  {...receipt, status: 'pending'}), /settle/);

const document = {revision: 'revision-a'};
assert.equal(applicationInterlock(document, {configRevision: 'revision-a',
  loadedRevision: 'revision-a', gainCheckPending: false, restart: {state: 'idle'}}), null);
assert.equal(applicationInterlock(document, {configRevision: 'revision-a',
  loadedRevision: 'revision-old', gainCheckPending: true, restart: {state: 'failed'}}),
  'configuration-restart-failed');
assert.equal(applicationInterlock(document, {configRevision: 'revision-a',
  loadedRevision: 'revision-old', gainCheckPending: true, restart: {state: 'running'}}),
  'configuration-restart-pending');
assert.equal(applicationInterlock(document, {configRevision: 'revision-a',
  loadedRevision: 'revision-a', gainCheckPending: false, restart: {state: 'scheduled'}}),
  'configuration-restart-pending');

const verifiedReceiver = {applicationVerified: true, reconciliationRequired: false,
  configRevision: 'revision-a'};
const skippedAfterGain = {configRevision: 'revision-a', loadedRevision: 'revision-a',
  gainCheckPending: true, restart: {state: 'idle'}, receiverSynchronization: verifiedReceiver};
assert.equal(applicationInterlock(document, skippedAfterGain, {processorActive: false}), null,
  'an inactive processor must be allowed to finish an exact verified gain restart');
assert.equal(applicationInterlock(document, skippedAfterGain, {processorActive: true}),
  'configuration-restart-pending', 'an active processor must finish gain verification normally');
assert.equal(applicationInterlock(document, {...skippedAfterGain,
  receiverSynchronization: {...verifiedReceiver, applicationVerified: false}},
{processorActive: false}), 'configuration-restart-pending',
'an unverified receiver must not bypass the gain restart interlock');
assert.equal(applicationInterlock(document, {...skippedAfterGain,
  receiverSynchronization: {...verifiedReceiver, configRevision: 'revision-old'}},
{processorActive: false}), 'configuration-restart-pending',
'a stale receiver receipt must not start the processor');
assert.equal(applicationInterlock(document, {...skippedAfterGain, restart: {state: 'running'}},
{processorActive: false}), 'configuration-restart-pending',
'an active configuration restart must retain the interlock');
assert.equal(applicationInterlock(document, {...skippedAfterGain, restart: {state: 'failed'}},
{processorActive: false}), 'configuration-restart-failed',
'a failed configuration restart must require repair');

let skippedState = evaluateFrameLiveness({now: 0, processorActive: false,
  processorPid: 0, lastFrameAt: null}, {}, {staleAfterMs: 1000}).state;
const skippedDecision = evaluateFrameLiveness({now: 1001, processorActive: false,
  processorPid: 0, lastFrameAt: null}, skippedState, {staleAfterMs: 1000});
assert.equal(applicationInterlock(document, skippedAfterGain, {processorActive: false}), null);
assert.equal(skippedDecision.action, 'start-processor',
  'a processor skipped during gain verification must be retriggered after exact receipt verification');

(async () => {
  // Exercise the real recovery/controller code with inert service/HTTP ports.
  // No test may stop or restart a host service.
  for (const receiver of ['Usrp', 'RspDuo', 'HackRF']) {
    const operations = [];
    let clock = 100000, active = true, revision = 'original', changeOnStop = false;
    let replay = false, publish = true, changeOnResponse = false;
    const sandbox = {__dirname, module: {exports: {}}, console,
      Date: class extends Date {static now() {return clock;}}, process,
      setTimeout(callback, delay) {clock += delay; queueMicrotask(callback);}};
    sandbox.require = name => {
      if (name.endsWith('/config-store')) return {readConfig: () => ({exists: true,
        validation: {valid: true}, revision, config: {capture: {device: {type: receiver},
          replay: {state: replay}}, network: {ip: '127.0.0.1', ports: {api: 1}}}})};
      if (name === 'child_process') return {execFileSync: (_file, args) => {
        operations.push(args.join(' '));
        if (args[0] === 'stop') {active = false; if (changeOnStop) revision = 'changed';}
        if (args[0] === 'start') active = true;
        if (args[0] === 'is-active' && !active) throw new Error('inactive');
        return args[0] === 'show' ? '42' : '';
      }};
      if (name === 'http') return {get: (_options, callback) => {
        const request = new EventEmitter(), response = new EventEmitter();
        response.statusCode = 200; response.setEncoding = () => {};
        queueMicrotask(() => {callback(response);
          if (changeOnResponse) revision = 'changed-during-response';
          response.emit('data', JSON.stringify({lastFrameAt: publish ? clock : 1}));
          response.emit('end');});
        return request;
      }};
      return require(name);
    };
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, 'vectorwarp-health-watchdog.js'), 'utf8'), sandbox);
    const {recover} = sandbox.module.exports;
    const result = await recover('/inert/config', 1);
    assert.equal(result.configRevision, 'original');
    assert.equal(result.firstRecoveredFrameAt, clock);
    assert.deepEqual(operations.slice(0, 2), ['stop vectorwarp-processor.service', 'start vectorwarp-processor.service']);
    assert(!operations.some(value => value.includes('heimdall')), 'SDK capture must not operate Kraken ingress');
    changeOnResponse = true;
    await assert.rejects(recover('/inert/config', 1), /configuration revision changed/);
    changeOnResponse = false; revision = 'original';
    operations.length = 0; changeOnStop = true;
    await assert.rejects(recover('/inert/config', 1), /configuration revision changed/);
    assert.deepEqual(operations, ['stop vectorwarp-processor.service']);
    operations.length = 0; replay = true;
    await assert.rejects(recover('/inert/config', 1), /limited to live input/);
    assert.deepEqual(operations, []);
    replay = false; changeOnStop = false; publish = false;
    const began = clock;
    await assert.rejects(recover('/inert/config', 1), /frames did not advance/);
    assert(clock - began <= FRAME_RECOVERY_TIMEOUT_MS, 'frame recovery stays bounded');
  }
  let attempts = 0, clock = 0;
  const recovered = await synchronizeAfterRestart(config, {
    factory: () => ({synchronize: async () => {
      attempts += 1;
      if (attempts < 3) {
        const error = new Error('socket not listening');
        error.code = 'KRAKEN_CONNECTION_FAILED';
        throw error;
      }
      return receipt;
    }}),
    now: () => clock,
    sleep: async milliseconds => { clock += milliseconds; },
    timeoutMs: 1000
  });
  assert.equal(attempts, 3);
  assert.deepEqual(recovered, receipt);

  attempts = 0;
  await assert.rejects(() => synchronizeAfterRestart(config, {
    factory: () => ({synchronize: async () => {
      attempts += 1;
      const error = new Error('receiver rejected settings');
      error.code = 'KRAKEN_REJECTED';
      throw error;
    }}),
    now: () => 0,
    sleep: async () => {},
    timeoutMs: 1000
  }), /receiver rejected settings/);
  assert.equal(attempts, 1);
  console.log('PASS exact RF preservation and bounded receiver-readiness retry');
})().catch(error => { console.error(error); process.exitCode = 1; });
