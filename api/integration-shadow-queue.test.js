'use strict';

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {IntegrationShadowQueue, digestCanonical, validTruthSelectedExports} = require('./integration-shadow-queue');
assert.equal(digestCanonical({zero: -0}), digestCanonical({zero: 0}),
  'signed receipt identity survives JSON signed-zero normalization');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-integration-shadow-'));
const receiptKeyPath = path.join(root, 'receipt.key');
fs.writeFileSync(receiptKeyPath, Buffer.alloc(32, 7), {mode: 0o600});
const config = {capture: {fc: 527000000, fs: 2400000, device: {channel_count: 2}},
  process: {detection: {integration_shadow: {enabled: true,
  queue_path: path.join(root, 'queue'), results_path: path.join(root, 'results'),
  receipt_key_path: receiptKeyPath,
  dwell_seconds: [.05, .1, .2, .5], fusion_modes: ['production', 'noncoherent'],
  max_pending: 1, max_wall_seconds_per_clip: 120}}}};
const queue = new IntegrationShadowQueue(config, {now: () => 1000});
const recording = path.join(root, 'one.mchq');
const nativeMetadata = {schema: 'vectorwarp-kraken-iq-clip-v1', frequency: 527000000,
  sampleRate: 2400000, channels: 2, channelGains: [2.7, 1.4],
  epoch: 1, firstEpochSample: 0, endEpochSample: 30};
fs.writeFileSync(recording, 'immutable-iq');
fs.writeFileSync(`${recording}.json`, JSON.stringify(nativeMetadata));
const admitted = {state: 'admitted', integrity: {valid: true, data_sha256: 'a'.repeat(64),
  sidecar_sha256: crypto.createHash('sha256').update(fs.readFileSync(`${recording}.json`)).digest('hex')},
  config_epoch: 'config', receiver_epoch: 'receiver', captures: {one: {capture_id: 'one',
    valid: true, admission_state: 'admitted', processor_cpis:
      Array.from({length: 3}, (_, i) => ({exact_event_time: true, timestamp_ms: 1000 + i * 500,
        epoch: 1, first_epoch_sample: i * 10, end_epoch_sample: (i + 1) * 10}))}}};

for (const exact of [false, undefined]) {
  const provisional = JSON.parse(JSON.stringify(admitted));
  provisional.captures.one.processor_cpis[0].exact_event_time = exact;
  assert.equal(queue.accept(recording, provisional, 3).reason, 'not-strictly-admitted',
    'shadow admission requires complete provisional timing provenance');
}

const first = queue.accept(recording, admitted, 4);
assert.equal(first.accepted, true);
const job = JSON.parse(fs.readFileSync(first.path));
assert.equal(job.experiment.shadow_only, true);
assert.equal(job.experiment.same_iq, true);
assert.equal(job.experiment.physical_channel_reference_policy, 'leave-one-out');
assert.equal(job.experiment.execution_host_policy, 'strix-only');
assert.deepEqual(job.experiment.dwell_seconds, [.05, .1, .2, .5]);
assert.match(job.identity.admission_sha256, /^[a-f0-9]{64}$/);
assert.match(job.identity.config_sha256, /^[a-f0-9]{64}$/);
assert.deepEqual(job.identity.native_identity.actual_channel_gains_db, [2.7, 1.4]);
assert.match(job.job_binding_sha256, /^[a-f0-9]{64}$/);
assert.equal(queue.accept(recording, admitted, 4).reason, 'duplicate');
const tooManyTruthPasses = JSON.parse(JSON.stringify(admitted));
for (let index = 1; index <= 8; ++index) tooManyTruthPasses.captures[`extra-${index}`] = {
  capture_id: `extra-${index}`, valid: true, admission_state: 'admitted',
  processor_cpis: Array.from({length: 3}, () => ({exact_event_time: true}))};
assert.equal(queue.accept(recording, tooManyTruthPasses, 4).reason, 'not-strictly-admitted',
  'each queued pass must fit the bounded truth-selected export budget');
fs.writeFileSync(`${recording}.json`, JSON.stringify({...nativeMetadata, channelGains: [2.7, 1.5]}));
assert.equal(queue.accept(recording, admitted, 4).reason, 'write-failed',
  'a sidecar whose decoded gain tuple differs from its admitted digest must fail closed');
fs.writeFileSync(`${recording}.json`, JSON.stringify(nativeMetadata));
assert.equal(queue.accept(recording, {...admitted,
  integrity: {...admitted.integrity, data_sha256: 'b'.repeat(64)}}, 5).reason, 'queue-full');
assert.equal(queue.accept(recording, {...admitted, state: 'quarantined'}, 6).reason,
  'not-strictly-admitted');
fs.mkdirSync(config.process.detection.integration_shadow.results_path, {recursive: true});
const truthSelectedExport = {schema: 'vectorwarp-truth-selected-shadow-export-v2',
  shadow_only: true, decision_grade: false, production_promotion_allowed: false,
  production_unchanged: true, selection: {basis: 'geometry-eligible-truth-cut-only',
    cfar_or_beam_selection_used: false, capture_id: 'one', arm: '0.1s-production'},
  provenance: {job_binding_sha256: job.job_binding_sha256,
    native_recorder: 'ResidentReplaySnapshot-v1', native_contract_verified: true,
    hardware_qualified: false, time_mapping: 'admitted-source-sample-midpoints'},
  multi_cpi_history: [1, 2, 3].map(frame => ({native_snapshot: {native_contract_verified: true,
    path: `/native/frame.${frame}.rps`, sha256: 'a'.repeat(64)},
    covariance_whitening: {state: 'native-resident-settled-snapshot'},
    target_cut: {physical_channels: [[1, 0]], beam_powers: [1]}})),
  covariance_whitening: {state: 'native-resident-settled-snapshot', estimated_from_cut: false}};
truthSelectedExport.export_sha256 = digestCanonical(truthSelectedExport);
assert.equal(validTruthSelectedExports([truthSelectedExport], job.job_binding_sha256), true);
assert.equal(validTruthSelectedExports([{...truthSelectedExport,
  schema: 'vectorwarp-truth-selected-shadow-export-v1'}], job.job_binding_sha256), false,
  'legacy CUT-estimated covariance cannot be accepted as native evidence');
assert.equal(validTruthSelectedExports([{...truthSelectedExport,
  selection: {...truthSelectedExport.selection, cfar_or_beam_selection_used: true}}],
job.job_binding_sha256), false, 'CFAR-selected shadow evidence must fail closed');
const validResult = {schema: 'vectorwarp-integration-shadow-result-v1',
  timing_policy: 'exact', decision_grade: false, timing_decision_grade: true, hardware_qualified: false,
  production_promotion_allowed: false,
  job_id: first.job_id, state: 'complete', shadow_only: true,
  job_binding_sha256: job.job_binding_sha256, production_unchanged: true,
  input: {recording_sha256: job.identity.recording_sha256,
    native_sidecar_sha256: job.identity.native_sidecar_sha256,
    admission_sha256: job.identity.admission_sha256,
    config_sha256: job.identity.config_sha256,
    native_identity: job.identity.native_identity,
    native_identity_sha256: job.identity.native_identity_sha256},
  truth_selected_exports: job.experiment.dwell_seconds.map(dwell => {
    const item = {...truthSelectedExport, selection: {...truthSelectedExport.selection, arm: `${dwell}s-production`}};
    delete item.export_sha256; item.export_sha256 = digestCanonical(item); return item;
  }), arms: {"0.1s": {production: {}}},
  timing: {wall_seconds: 2}, summary: {winner: null, selection_performed: false}};
assert.equal(validTruthSelectedExports(validResult.truth_selected_exports, job.job_binding_sha256, job), true);
assert.equal(validTruthSelectedExports(validResult.truth_selected_exports.slice(1), job.job_binding_sha256, job), false,
  'a signed result must cover every queued pass and dwell');
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
const receiptFile = path.join(config.process.detection.integration_shadow.results_path,
  `${first.job_id}.result.json`);
const savedReceipt = fs.readFileSync(receiptFile);
const oversized = fs.openSync(receiptFile, 'w');
fs.ftruncateSync(oversized, 16 * 1024 * 1024 + 1); fs.closeSync(oversized);
assert.equal(queue.resultRecords()[0].valid, false, 'oversized receipts are rejected before JSON allocation');
fs.writeFileSync(receiptFile, savedReceipt);
assert.equal(queue.resultRecords()[0].valid, true, 'changed receipt metadata invalidates the compact status cache');
const provisionalConfig = JSON.parse(JSON.stringify(config));
provisionalConfig.process.detection.integration_shadow.queue_path = path.join(root, 'provisional-queue');
const provisionalQueue = new IntegrationShadowQueue(provisionalConfig);
const provisionalAdmission = JSON.parse(JSON.stringify(admitted));
provisionalAdmission.captures.one.processor_cpis = [1000, 1500, 2000].map((timestamp, index) => ({
  timestamp_ms: timestamp, epoch: 1, first_epoch_sample: index * 10, end_epoch_sample: (index + 1) * 10,
  exact_event_time: false, event_time_source: 'kraken_heimdall_host_arrival_estimate',
  event_time_uncertainty_s: 1, event_time_uncertainty_model: 'bounded-configured-nuisance-radius'}));
provisionalAdmission.captures.one.truth_samples = [1000, 1500, 2000].map(timestamp => ({
  timestamp_ms: timestamp, delay_km: 10, doppler_hz: 20, delay_rate_km_s: .1, doppler_rate_hz_s: 7,
  provenance: {projection_model: 'constant_velocity_ground_track', observation_time_offset_s: 0,
    maximum_pairwise_projected_disagreement_km: 0}}));
const provisionalJobResult = provisionalQueue.accept(recording, provisionalAdmission, 20);
assert.equal(provisionalJobResult.accepted, true, 'qualified provisional capture must produce useful shadow work');
const provisionalJob = JSON.parse(fs.readFileSync(provisionalJobResult.path));
assert.equal(provisionalJob.timing_policy, 'provisional');
assert.equal(provisionalJob.decision_grade, false);
const outside = JSON.parse(JSON.stringify(provisionalAdmission));
outside.captures.one.processor_cpis[2].end_epoch_sample = 31;
assert.equal(provisionalQueue.accept(recording, outside, 21).reason, 'write-failed',
  'provisional admission must retain native sample containment');
const wrongGainResult = JSON.parse(JSON.stringify(validResult));
wrongGainResult.input.native_identity.actual_channel_gains_db[1] = 9.9;
wrongGainResult.result_binding_sha256 = digestCanonical(Object.fromEntries(
  Object.entries(wrongGainResult).filter(([key]) => !['result_binding_sha256', 'result_authentication'].includes(key))));
const wrongAuthenticated = {...wrongGainResult}; delete wrongAuthenticated.result_authentication;
wrongGainResult.result_authentication.tag = crypto.createHmac('sha256', fs.readFileSync(receiptKeyPath))
  .update(require('./integration-shadow-queue').canonical(wrongAuthenticated)).digest('hex');
fs.writeFileSync(path.join(config.process.detection.integration_shadow.results_path,
  `${first.job_id}.result.json`), JSON.stringify(wrongGainResult));
assert.equal(queue.status().completed, 0, 'a signed but incompatible gain receipt must fail closed');
fs.writeFileSync(path.join(config.process.detection.integration_shadow.results_path,
  `${first.job_id}.result.json`), JSON.stringify(validResult));
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
