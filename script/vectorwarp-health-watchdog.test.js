'use strict';
const assert = require('assert');
const {receiverSettlement, requested, sameSettings, synchronizeAfterRestart, applicationInterlock} =
  require('./vectorwarp-health-watchdog');

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

(async () => {
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
