'use strict';

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {IntegrationShadowQueue, digestCanonical} = require('./integration-shadow-queue');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-integration-shadow-'));
const receiptKeyPath = path.join(root, 'receipt.key');
fs.writeFileSync(receiptKeyPath, Buffer.alloc(32, 7), {mode: 0o600});
const config = {process: {detection: {integration_shadow: {enabled: true,
  queue_path: path.join(root, 'queue'), results_path: path.join(root, 'results'),
  receipt_key_path: receiptKeyPath,
  dwell_seconds: [.05, .1, .2, .5], fusion_modes: ['production', 'noncoherent'],
  max_pending: 1, max_wall_seconds_per_clip: 120}}}};
const queue = new IntegrationShadowQueue(config, {now: () => 1000});
const admitted = {state: 'admitted', integrity: {valid: true, data_sha256: 'a'.repeat(64),
  sidecar_sha256: 'c'.repeat(64)},
  config_epoch: 'config', receiver_epoch: 'receiver', captures: {one: {capture_id: 'one',
    valid: true, admission_state: 'admitted', processor_cpis: [{}, {}, {}]}}};

const first = queue.accept('/recordings/one.mchq', admitted, 4);
assert.equal(first.accepted, true);
const job = JSON.parse(fs.readFileSync(first.path));
assert.equal(job.experiment.shadow_only, true);
assert.equal(job.experiment.same_iq, true);
assert.equal(job.experiment.physical_channel_reference_policy, 'leave-one-out');
assert.deepEqual(job.experiment.dwell_seconds, [.05, .1, .2, .5]);
assert.match(job.identity.admission_sha256, /^[a-f0-9]{64}$/);
assert.match(job.identity.config_sha256, /^[a-f0-9]{64}$/);
assert.match(job.job_binding_sha256, /^[a-f0-9]{64}$/);
assert.equal(queue.accept('/recordings/one.mchq', admitted, 4).reason, 'duplicate');
assert.equal(queue.accept('/recordings/two.mchq', {...admitted,
  integrity: {...admitted.integrity, data_sha256: 'b'.repeat(64)}}, 5).reason, 'queue-full');
assert.equal(queue.accept('/recordings/bad.mchq', {...admitted, state: 'quarantined'}, 6).reason,
  'not-strictly-admitted');
fs.mkdirSync(config.process.detection.integration_shadow.results_path, {recursive: true});
const validResult = {schema: 'vectorwarp-integration-shadow-result-v1',
  job_id: first.job_id, state: 'complete', shadow_only: true,
  job_binding_sha256: job.job_binding_sha256, production_unchanged: true,
  input: {recording_sha256: job.identity.recording_sha256,
    native_sidecar_sha256: job.identity.native_sidecar_sha256,
    admission_sha256: job.identity.admission_sha256,
    config_sha256: job.identity.config_sha256}, arms: {"0.1s": {production: {}}},
  timing: {wall_seconds: 2}, summary: {winner: null, selection_performed: false}};
validResult.result_binding_sha256 = digestCanonical(validResult);
validResult.result_authentication = {algorithm: 'hmac-sha256',
  key_id: crypto.createHash('sha256').update(fs.readFileSync(receiptKeyPath)).digest('hex')};
const authenticatedResult = {...validResult}; delete authenticatedResult.result_authentication;
validResult.result_authentication.tag = crypto.createHmac('sha256', fs.readFileSync(receiptKeyPath))
  .update(require('./integration-shadow-queue').canonical(authenticatedResult)).digest('hex');
fs.writeFileSync(path.join(config.process.detection.integration_shadow.results_path,
  `${first.job_id}.result.json`), JSON.stringify(validResult));
const status = queue.status();
assert.equal(status.pending, 0);
assert.equal(status.completed, 1);
assert.equal(status.reserved_slots, 0);
assert.equal(status.production_path_changed, false);
const tamperedResult = JSON.parse(JSON.stringify(validResult));
tamperedResult.summary.selection_reason = 'forged';
fs.writeFileSync(path.join(config.process.detection.integration_shadow.results_path,
  `${first.job_id}.result.json`), JSON.stringify(tamperedResult));
assert.equal(queue.status().completed, 0, 'a spool writer must not be able to forge completion');
fs.writeFileSync(path.join(config.process.detection.integration_shadow.results_path,
  `${first.job_id}.result.json`), JSON.stringify(validResult));
fs.writeFileSync(path.join(config.process.detection.integration_shadow.results_path,
  'forged.result.json'), '{}');
assert.equal(queue.status().invalid_results, 1,
  'an invalid filename-only result must never release a queue slot');
const liveReservation = path.join(config.process.detection.integration_shadow.queue_path, 'live.slot');
fs.writeFileSync(liveReservation, JSON.stringify({job_id: 'e'.repeat(64), owner_pid: process.pid,
  owner_host: os.hostname(), owner_nonce: 'f'.repeat(32), created_at: 0}));
fs.utimesSync(liveReservation, new Date(0), new Date(0));
queue.reconcileSlots();
assert.equal(fs.existsSync(liveReservation), true,
  'age alone must never reclaim a reservation whose same-host owner is alive');
console.log('PASS bounded external integration shadow queue');
