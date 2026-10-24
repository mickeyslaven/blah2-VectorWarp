'use strict';
const assert = require('assert');
const {receiverSettlement, requested, sameSettings} = require('./vectorwarp-health-watchdog');

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
console.log('PASS exact RF preservation and idempotent revision settlement');
