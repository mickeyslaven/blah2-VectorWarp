'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {CausalFunnelRecorder, validateFrame} = require('./causal-funnel-recorder');
const {createCausalFunnelProducer, createMultiIlluminatorCausalFrame} =
  require('./causal-funnel-producer');
const {validateConfig} = require('./config-manager');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-causal-funnel-'));
const config = {process: {evidence: {causal_funnel: {enabled: true,
  epoch_id: 'causal-test-epoch', path: directory, max_candidates: 2,
  max_queue: 2, max_queue_mb: 1, max_file_mb: 1, max_files: 2}}}};
const frame = sequence => ({schema: 'vectorwarp-causal-funnel-frame-v1', source: 'synthetic',
  event_time_ns: 1000000 + sequence, sequence, stage: 'candidate_generation', reason: 'interval', candidates: [{id: `cell-${sequence}`,
    delay_km: 12.5, doppler_hz: -24.5, snr_db: 8.25,
    bearing_candidates_deg: [12, 192]}]});
const lineage = {epoch: '1', generation: '9007199254740993', consumed_sequence: '7',
  clock_precision_ns: '1000000', input_epoch: '0', first_sample: '1200000', end_sample: '2400000'};
const frame2 = () => ({schema: 'vectorwarp-causal-funnel-frame-v2', source: 'native',
  event_time_ns: '1794521720793000001', sequence: '9007199254740993',
  stage: 'tracker_frame', reason: 'interval', lineage,
  coverage: {candidates_total: 1, tracks_total: 0, candidates_retained: 1, tracks_retained: 0, truncated: false},
  candidates: [{id: 'c7-0', kind: 'candidate', delay_km: 6.5, doppler_hz: -226.05,
    snr_db: 5.22, bearing_candidates_deg: [], reason: 'seeded', state: 0,
    previous_state: 0, measured: true, confirmation_credit: true, support_depth: 2}]});

assert.equal(validateFrame(JSON.parse(JSON.stringify(frame2()))).event_time_ns, '1794521720793000001');
for (const value of [1794521720793000000, '-1', '+1', '01', '1e18', '1.0', '',
  '18446744073709551616', '9'.repeat(100), null])
  assert.throws(() => validateFrame({...frame2(), event_time_ns: value}), /Invalid/);
assert.doesNotThrow(() => validateFrame({...frame2(), event_time_ns: '18446744073709551615'}));
assert.throws(() => validateFrame({...frame(0), event_time_ns: 1794521720793000000}), /Invalid/);
assert.throws(() => validateFrame({...frame(0), event_time_ns: '1000000'}), /Invalid/);
assert.throws(() => validateFrame({...frame(0), stage: 'tracker_frame'}), /Invalid/);
assert.throws(() => validateFrame({...frame(0), reason: 'duplicate', candidates: []}), /Invalid/);
assert.doesNotThrow(() => validateFrame({...frame2(), reason: 'duplicate', candidates: [], coverage: undefined}));
assert.throws(() => validateFrame({...frame2(), reason: 'duplicate'}), /Invalid/);
assert.throws(() => validateFrame({...frame2(), lineage: {...lineage, generation: 9007199254740992}}), /lineage/);
assert.throws(() => validateFrame({...frame2(), lineage: {...lineage, end_sample: '1'}}), /interval/);
assert.throws(() => validateFrame({...frame2(), lineage: {...lineage, truth_epoch: '1'}}), /Truth-shaped/);
assert.throws(() => validateFrame({...frame2(), coverage: {...frame2().coverage, tracks_retained: 1}}), /coverage/);
assert.throws(() => validateFrame({...frame2(), candidates: [{...frame2().candidates[0], reason: 'truth_matched'}]}), /metadata/);
assert.throws(() => validateFrame({...frame2(), candidates: [{...frame2().candidates[0], delay_gate_km: Infinity}]}), /residual/);
const seeded = {...frame2().candidates[0], spawned_first_track_id: 't65534', spawned_track_count: 2};
assert.doesNotThrow(() => validateFrame({...frame2(), candidates: [seeded]}));
for (const change of [{spawned_first_track_id: 't65535'}, {spawned_track_count: 0},
  {confirmation_credit: false}, {kind: 'track'}, {reason: 'capacity'}])
  assert.throws(() => validateFrame({...frame2(), candidates: [{...seeded, ...change}]}), /seed linkage/);
assert.doesNotThrow(() => validateFrame({...frame2(), candidates: [{...frame2().candidates[0],
  reason: 'associated', confirmation_credit: null}]}));
assert.throws(() => validateFrame({...frame2(), candidates: [{...frame2().candidates[0],
  kind: 'track', confirmation_credit: null}]}), /metadata/);

const multiFrame = createMultiIlluminatorCausalFrame({id: 'WTAE-RF27',
  timestamp: 1794521720793, frequency_hz: 587000000, sample_rate_hz: 2000000,
  first_input_sample: 1200000, end_input_sample: 2200000,
  capture_sample_position_available: true, capture_epoch: 3,
  capture_first_input_sample: 91200000, capture_end_input_sample: 92200000,
  detections: {delay: [5.25, 8.5, 12.75], doppler: [-35, 12, 44],
    snr: [6.5, 7.25, 8.75]},
  trackerDiagnostics: {detections: 3, tracks_before: 1, tracks_after: 2,
    candidate_track_pairs: 3, pair_in_gate: 1, associations: 1, misses: 0,
    birth_candidates: 2, initiated: 1, no_op: false}}, 2);
assert.equal(validateFrame(JSON.parse(JSON.stringify(multiFrame))).channel, 'WTAE-RF27');
assert.deepEqual(multiFrame.coverage, {candidates_total: 3, tracks_total: 2,
  candidates_retained: 2, tracks_retained: 0, truncated: true});
assert.equal(multiFrame.lineage.input_epoch, '3');
assert.equal(multiFrame.tracker_diagnostics.associations, 1);
assert.throws(() => createMultiIlluminatorCausalFrame({
  ...multiFrame, id: 'WTAE RF27'}), /identity/);
assert.throws(() => validateFrame({...multiFrame,
  tracker_diagnostics: {...multiFrame.tracker_diagnostics, adsb_matches: 1}}),
  /Truth-shaped/);

(async () => {
  const recorder = new CausalFunnelRecorder(config, {now: () => 1234});
  assert.equal(recorder.accept(frame(0)), true);
  await recorder.flush();
  const manifest = path.join(directory, 'causal-test-epoch.manifest.json');
  const data = path.join(directory, 'causal-test-epoch-000000.jsonl');
  assert.equal(fs.statSync(manifest).mode & 0o777, 0o600);
  assert.equal(fs.statSync(data).mode & 0o777, 0o600);
  const row = JSON.parse(fs.readFileSync(data, 'utf8'));
  assert.equal(row.frame.source, 'synthetic');
  assert.equal(row.epoch_id, 'causal-test-epoch');
  assert.equal(recorder.status().written, 1);
  assert.equal(recorder.status().truth_blind, true);
  const apiOnlyConfig = JSON.parse(JSON.stringify(config));
  apiOnlyConfig.process.evidence.causal_funnel.enabled = false;
  apiOnlyConfig.process.evidence.causal_funnel.api_ingest_enabled = true;
  apiOnlyConfig.process.evidence.causal_funnel.epoch_id = 'api-only-causal-test';
  apiOnlyConfig.process.evidence.causal_funnel.path = path.join(directory, 'api-only');
  const apiOnly = new CausalFunnelRecorder(apiOnlyConfig);
  assert.equal(apiOnly.status().enabled, true);
  assert.equal(apiOnly.accept(multiFrame), true);
  await apiOnly.flush();
  assert.equal(apiOnly.status().written, 1);
  assert.throws(() => validateFrame({...frame(1), truth: {icao: 'abc123'}}), /Truth-shaped field/);
  assert.throws(() => validateFrame({...frame(1), candidates: [{...frame(1).candidates[0], latitude: 1}]}), /Truth-shaped field/);
  assert.doesNotThrow(() => validateFrame({...frame(1), source: 'native'}));
  assert.doesNotThrow(() => validateFrame({...frame(1), reason: 'no_candidates', candidates: []}));
  assert.throws(() => validateFrame({...frame(1), unexpected: true}), /Unknown causal funnel field/);
  assert.equal(recorder.accept(JSON.stringify({...frame(1), ignored: 'x'.repeat(2 * 1024 * 1024)})), false);
  assert.match(recorder.status().error, /byte bound/);
  const producer = createCausalFunnelProducer(recorder,
    {buildId: 'development-0000000', configFingerprint: 'funnel-config-v1'});
  assert.equal(producer.emit({eventTimeNs: 1010000, stage: 'candidate_generation',
    reason: 'no_candidates', lineage}), true);
  assert.equal(producer.emit({eventTimeNs: 1794521720793000000, stage: 'candidate_generation',
    reason: 'no_candidates', lineage}), false, 'rounded Number must not be normalized');
  await recorder.flush();

  // Queue pressure drops only the newest frame and reports it; production flow is unaffected.
  recorder.writing = true;
  assert.equal(recorder.accept(frame(2)), true);
  assert.equal(recorder.accept(frame(3)), true);
  assert.equal(recorder.accept(frame(4)), false);
  assert.equal(recorder.status().dropped, 1);
  recorder.writing = false;
  await recorder.flush();
  assert.equal(recorder.status().written, 4);

  // Rotation creates a new append-only segment and retention removes only the
  // oldest completed segment; neither operation rewrites a prior record.
  fs.writeFileSync(data, Buffer.alloc(1024 * 1024));
  assert.equal(recorder.accept(frame(5)), true);
  await recorder.flush();
  const segmentOne = path.join(directory, 'causal-test-epoch-000001.jsonl');
  assert.ok(fs.existsSync(segmentOne));
  fs.writeFileSync(segmentOne, Buffer.alloc(1024 * 1024));
  assert.equal(recorder.accept(frame(6)), true);
  await recorder.flush();
  assert.ok(fs.existsSync(path.join(directory, 'causal-test-epoch-000002.jsonl')));
  assert.ok(!fs.existsSync(data));
  assert.equal(recorder.status().retained_files, 2);
  const resumed = new CausalFunnelRecorder(config);
  assert.equal(resumed.accept(frame(7)), true);
  await resumed.flush();
  assert.match(fs.readFileSync(path.join(directory, 'causal-test-epoch-000002.jsonl'), 'utf8'), /"sequence":7/);

  // Epoch manifests bind the immutable recorder configuration.
  const changed = JSON.parse(JSON.stringify(config));
  changed.process.evidence.causal_funnel.max_queue = 3;
  const incompatible = new CausalFunnelRecorder(changed);
  assert.equal(incompatible.accept(frame(8)), true);
  await incompatible.flush();
  assert.match(incompatible.status().error, /immutable/);

  const failingConfig = JSON.parse(JSON.stringify(config));
  failingConfig.process.evidence.causal_funnel.path = path.join(directory, 'failed-write');
  const failingFs = {promises: {...fs.promises, appendFile: async () => {
    throw new Error('injected append failure');
  }}};
  const failing = new CausalFunnelRecorder(failingConfig, {fileSystem: failingFs});
  assert.equal(failing.accept(frame(9)), true);
  await failing.flush();
  assert.equal(failing.status().written, 0);
  assert.equal(failing.status().dropped, 1, 'the in-flight record is accounted as dropped');
  assert.match(failing.status().error, /injected append failure/);

  // A metadata refresh happens only after the append has completed. It must
  // surface an operational error without retroactively dropping the durable row.
  const metadataFailureConfig = JSON.parse(JSON.stringify(config));
  metadataFailureConfig.process.evidence.causal_funnel.path = path.join(directory, 'post-append-metadata');
  let readdirCalls = 0;
  const metadataFailureFs = {promises: {...fs.promises, readdir: async (...arguments_) => {
    readdirCalls += 1;
    if (readdirCalls === 3) throw new Error('injected post-append metadata read failure');
    return fs.promises.readdir(...arguments_);
  }}};
  const metadataFailure = new CausalFunnelRecorder(metadataFailureConfig, {fileSystem: metadataFailureFs});
  assert.equal(metadataFailure.accept(frame(10)), true);
  await metadataFailure.flush();
  assert.equal(metadataFailure.status().written, 1);
  assert.equal(metadataFailure.status().dropped, 0);
  assert.match(metadataFailure.status().error, /injected post-append metadata read failure/);
  assert.match(fs.readFileSync(path.join(metadataFailureConfig.process.evidence.causal_funnel.path,
    'causal-test-epoch-000000.jsonl'), 'utf8'), /"sequence":10/);

  // A complete normal configuration validates disabled-by-default funnel bounds.
  const full = {capture: {fs: 2000000, fc: 100000000, device: {type: 'RspDuo',
    agcSetPoint: -20, bandwidthNumber: 5, serial: '', gainReduction: [50, 45], lnaState: 1,
    dabNotch: false, rfNotch: false}, replay: {state: false, loop: false, file: '/tmp/input.blah2iq'}}, process: {data: {cpi: .5, buffer: 2},
    ambiguity: {delayMin: -10, delayMax: 400, dopplerMin: -200, dopplerMax: 200},
    clutter: {enable: true, followAmbiguity: true}, detection: {enable: true, pfa: .001,
      nGuard: 2, nTrain: 4, nCentroid: 1, minDelay: 1, minDoppler: 1}, tracker: {enable: false,
      delete: 1, initiate: {M: 1, N: 1, maxAcc: 1}}, evidence: config.process.evidence},
    network: {ip: '127.0.0.1', ports: {api: 3000, map: 3001, detection: 3002, track: 3003,
      timestamp: 4000, timing: 4001, iqdata: 4002, config: 4003, causal_funnel: 3008}},
    location: {rx: {latitude: 1, longitude: 1, altitude: 1, name: 'rx'},
      tx: {latitude: 2, longitude: 2, altitude: 1, name: 'tx'}}, truth: {adsb: {enabled: false, tar1090: 'localhost:8080'}},
    save: {iq: false, map: false, detection: false, timing: false, path: '/tmp/', retention: {}}};
  assert.equal(validateConfig(full).valid, true, validateConfig(full).errors.join('; '));
  full.process.evidence.causal_funnel.epoch_id = 'bad';
  assert.equal(validateConfig(full).valid, false);
  console.log('Causal funnel recorder: truth-blind schema, immutable epoch, bounded queue, and secure append-only files passed.');
})().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  fs.rmSync(directory, {recursive: true, force: true});
});
