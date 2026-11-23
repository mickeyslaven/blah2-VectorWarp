'use strict';
// Admission regression: configuration intent survives ingress/backend changes;
// a package/config hash or advancing frames cannot attest a missing feature.
const assert = require('node:assert/strict');
const {test} = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const {spawnSync} = require('node:child_process');
const {effectiveRadarPlan, radarPlanErrors, validateConfig, assessResolvedRadarPlans} = require('./config-manager');
const {RadarSessionStore} = require('./presentation-history');
const {readConfig, saveConfig, configurationForReceiver, migrateKnownDefaults} = require('./config-store');
const {checkReceiverStart} = require('./receiver-start-check');
const profiles = require('../html/js/config_profiles');
const clone = value => JSON.parse(JSON.stringify(value));
function configured(depth = 2) {
  const config = configurationForReceiver('Usrp');
  profiles.applyWeakTargetMode(config, depth === 1 ? 'off' : depth === 2 ? 'two' : depth === 4 ? 'four' : 'eight');
  Object.assign(config.process.tracker.initiate, {M: 3, N: 1 + depth * 2});
  config.process.tracker.delete = Math.max(3, depth - 1);
  return config;
}
function pin(config) {
  return {schema: 'vectorwarp-radar-plan-requirements-v1',
    semantic_sha256: effectiveRadarPlan(config).semantic_sha256,
    required_features: {detection: true, tracking: true,
      temporal_cpis: config.process.detection.track_before_detect.enable ?
        config.process.detection.track_before_detect.max_cpis : config.process.detection.integration_cpis}};
}
function fixture(run) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'radar-plan-'));
  const filename = path.join(directory, 'config.json');
  const config = configured();
  fs.writeFileSync(filename, JSON.stringify(config));
  try { return run({filename, config, contract: `${filename}.radar-plan.json`}); }
  finally { fs.rmSync(directory, {recursive: true, force: true}); }
}

test('semantic hash excludes backend, OS paths, live/replay selection and truth', () => {
  const config = configured(4), original = JSON.stringify(config);
  const expected = effectiveRadarPlan(config).semantic_sha256;
  for (const acceleration of ['auto', 'cpu', 'gpu', 'gpu-resident'])
    for (const input of ['live', 'replay'])
      for (const type of ['Usrp', 'RspDuo', 'HackRF', 'Kraken']) {
        const candidate = clone(config);
        candidate.capture.device.type = type;
        if (type === 'Kraken') Object.assign(candidate.capture.device,
          {channel_count: 2, reference_channel: 0, surveillance_channels: [1]});
        candidate.capture.replay.state = input === 'replay';
        candidate.capture.replay.file = '/different/operating/system/recording.blah2iq';
        candidate.process.performance.acceleration = acceleration;
        candidate.process.performance.fft_threads = 8;
        candidate.truth.adsb.enabled = true;
        candidate.truth.adsb.tar1090 = 'another-truth-source';
        candidate.location.rx.name = 'another display label';
        assert.equal(effectiveRadarPlan(candidate).semantic_sha256, expected, `${type}/${acceleration}/${input}`);
      }
  assert.equal(JSON.stringify(config), original, 'inspection must never mutate settings');
});

test('recorded topology overrides dormant live roles; physical mapping and geometry remain bound', () => {
  const live = configured(), replay = clone(live);
  Object.assign(replay.capture.replay, {state: true, channel_count: 2,
    reference_channel: 0, reference_channels: [0], surveillance_channels: [1], reference_mode: 'dedicated'});
  replay.capture.device = {type: 'Kraken', channel_count: 5,
    reference_channel: 3, surveillance_channels: [0, 1, 2, 4]};
  assert.equal(effectiveRadarPlan(replay).semantic_sha256, effectiveRadarPlan(live).semantic_sha256);
  replay.capture.replay.reference_channel = 1;
  replay.capture.replay.reference_channels = [1];
  replay.capture.replay.surveillance_channels = [0];
  assert.notEqual(effectiveRadarPlan(replay).semantic_sha256, effectiveRadarPlan(live).semantic_sha256);
  const moved = clone(live); moved.location.tx.latitude = 10;
  assert.notEqual(effectiveRadarPlan(moved).semantic_sha256, effectiveRadarPlan(live).semantic_sha256);
});

test('direct and channelized ingress share the same effective radar contract', () => {
  const direct = configured(4), channelized = clone(direct);
  channelized.process.multi_illuminator = {enabled: true, illuminators: [{enabled: true,
    id: 'arbitrary-label', standard: 'FM', frequency_hz: direct.capture.fc,
    output_sample_rate_hz: direct.capture.fs}]};
  assert.equal(effectiveRadarPlan(channelized).semantic_sha256, effectiveRadarPlan(direct).semantic_sha256);
  channelized.process.multi_illuminator.illuminators[0].output_sample_rate_hz /= 2;
  assert.notEqual(effectiveRadarPlan(channelized).semantic_sha256, effectiveRadarPlan(direct).semantic_sha256,
    'a different physical sample grid is not semantic parity');
});

test('lane reference conditioning, initiation threshold and both transmitter geometries bind intent', () => {
  const config = configured(8);
  config.process.multi_illuminator = {enabled: true, illuminators: [{id: 'atsc', enabled: true,
    standard: 'ATSC-1.0', frequency_hz: config.capture.fc, output_sample_rate_hz: config.capture.fs,
    atsc_reference_mode: 'pilot_aided', transmitter: {name: 'Site', latitude: 40, longitude: -79, altitude: 300},
    transmitter_location: {latitude: 40.1, longitude: -79.1, altitude: 310}}]};
  const original = effectiveRadarPlan(config).semantic_sha256;
  for (const mutate of [lane => {lane.atsc_reference_mode = 'raw';},
    lane => {lane.tracker_min_snr_db = 12;},
    lane => {lane.transmitter.latitude++;}, lane => {lane.transmitter_location.latitude++;}]) {
    const changed = clone(config); mutate(changed.process.multi_illuminator.illuminators[0]);
    assert.notEqual(effectiveRadarPlan(changed).semantic_sha256, original);
    assert.ok(radarPlanErrors(effectiveRadarPlan(changed), pin(config)).some(error => error.includes('differs')));
  }
  const equivalent = clone(config), lane = equivalent.process.multi_illuminator.illuminators[0];
  lane.atsc_reference_mode = 'automatic'; lane.standard = 'ATSC'; lane.transmitter.name = 'Display label';
  lane.tracker_min_snr_db = equivalent.process.tracker.initiate.min_snr_db;
  assert.equal(effectiveRadarPlan(equivalent).semantic_sha256, original,
    'effective conditioning, threshold and geometry matter; aliases and labels do not');
  const fm = configured(8);
  fm.process.multi_illuminator = {enabled: true, illuminators: [{id: 'fm', enabled: true,
    standard: 'FM', frequency_hz: fm.capture.fc, output_sample_rate_hz: fm.capture.fs}]};
  const raw = effectiveRadarPlan(fm).semantic_sha256;
  fm.process.multi_illuminator.experimental_fm_reference_quality_filter = true;
  assert.notEqual(effectiveRadarPlan(fm).semantic_sha256, raw);
});

test('alias spelling, inactive options and key order normalize; changed behavior does not', () => {
  const config = configured(4), expected = effectiveRadarPlan(config).semantic_sha256;
  const tbd = config.process.detection.track_before_detect;
  tbd.delay_motion_bins = tbd.motion_bins; tbd.doppler_motion_bins = tbd.motion_bins;
  delete tbd.motion_bins;
  config.process.detection.integration_cpis = 1; // Deep TBD supplies the active depth.
  config.process = Object.fromEntries(Object.entries(config.process).reverse());
  assert.equal(effectiveRadarPlan(config).semantic_sha256, expected);
  config.process.detection.track_before_detect.enable = false;
  assert.notEqual(effectiveRadarPlan(config).semantic_sha256, expected);
});

test('all required features are checked per lane, even with a self-matching wrong hash', () => {
  const intended = pin(configured(4)), missing = effectiveRadarPlan(configured(1));
  assert.ok(radarPlanErrors(missing, intended).some(error => error.includes('differs')));
  intended.semantic_sha256 = missing.semantic_sha256;
  assert.ok(radarPlanErrors(missing, intended).some(error => error.includes('temporal_cpis=4')));
  intended.required_features = {typo: true};
  assert.ok(radarPlanErrors(missing, intended).some(error => error.includes('typo')));
  assert.ok(radarPlanErrors(missing, {...intended, unexpected: true}).includes('Required radar plan is malformed'));
  assert.ok(radarPlanErrors(missing, {...intended, required_features: {}}).length,
    'a bare matching hash is insufficient without an explicit required behavior');
});

test('enabled eight-CPI TBD overrides single-CPI integration on every lane and refuses a stale one-CPI pin', () => {
  const config = configured(8);
  config.process.detection.integration_cpis = 1;
  config.process.multi_illuminator = {enabled: true, illuminators: ['a', 'b', 'c'].map(id =>
    ({enabled: true, id, standard: 'FM', frequency_hz: config.capture.fc,
      output_sample_rate_hz: config.capture.fs}))};
  const report = effectiveRadarPlan(config), requirement = pin(config);
  assert.deepEqual(report.lanes.map(lane => lane.features.temporal_cpis), [8, 8, 8]);
  assert.deepEqual(report.lanes.map(lane => lane.configured_products.integrationCpis), [8, 8, 8]);
  assert.deepEqual(radarPlanErrors(report, requirement), []);
  requirement.required_features.temporal_cpis = 1;
  assert.deepEqual(radarPlanErrors(report, requirement), ['a', 'b', 'c'].map(id =>
    `Required radar feature temporal_cpis=1 is absent in lane ${id} (configured temporal_cpis=8)`));
});

test('every temporal depth retains valid independent confirmation requirements', () => {
  for (const depth of [1, 2, 4, 8]) {
    const config = configured(depth), before = JSON.stringify(config);
    assert.equal(validateConfig(config).valid, true, validateConfig(config).errors.join('; '));
    effectiveRadarPlan(config);
    assert.equal(JSON.stringify(config), before);
    if (depth > 1) {
      config.process.tracker.initiate.N--;
      assert.equal(validateConfig(config).valid, false);
    }
  }
});

test('read/save gate rejects disabled promotion without writing or modifying the pin', () => fixture(({filename, contract}) => {
  const first = readConfig(filename);
  assert.equal(first.radarPlan.qualification, 'unpinned');
  fs.writeFileSync(contract, JSON.stringify(pin(first.config)));
  const pinned = readConfig(filename), before = fs.readFileSync(filename, 'utf8');
  assert.equal(pinned.radarPlan.qualification, 'matched');
  const candidate = clone(pinned.config);
  candidate.process.detection.integration_cpis = 1;
  assert.throws(() => saveConfig(filename, candidate, pinned.revision), /Required radar feature temporal_cpis=2/);
  assert.equal(fs.readFileSync(filename, 'utf8'), before);
  candidate.process.detection.integration_cpis = 2;
  candidate.network.ports.api++;
  assert.equal(saveConfig(filename, candidate, pinned.revision).radarPlan.qualification, 'matched');
  fs.writeFileSync(contract, '{broken');
  assert.equal(readConfig(filename).validation.valid, false);
}));

test('explicit requirement cannot disappear, including when replay skips receiver control', async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'radar-start-plan-'));
  try {
    const filename = path.join(directory, 'config.json'), config = configured();
    config.capture.replay.state = true;
    fs.writeFileSync(filename, JSON.stringify(config));
    const missing = path.join(directory, 'required.json');
    const document = readConfig(filename, {requiredPlanFile: missing});
    assert.equal(document.radarPlan.qualification, 'rejected');
    await assert.rejects(checkReceiverStart(filename, {readConfig: () => document}), /Radar plan admission failed/);
  } finally { fs.rmSync(directory, {recursive: true, force: true}); }
});

test('startup migration cannot widen pinned temporal confirmation', () => fixture(({filename, contract}) => {
  const config = configured(4);
  config.process.tracker.initiate.N = 6;
  fs.writeFileSync(filename, JSON.stringify(config));
  fs.writeFileSync(contract, JSON.stringify(pin(readConfig(filename).config)));
  const before = fs.readFileSync(filename, 'utf8');
  assert.throws(() => migrateKnownDefaults(filename, {report() {}}), /pinned intended plan/);
  assert.equal(fs.readFileSync(filename, 'utf8'), before);
}));

test('CLI acceptance requires a real independently supplied requirement', () => fixture(({filename, contract}) => {
  const cli = (...args) => spawnSync(process.execPath, [path.join(__dirname, 'config-store.js'), ...args],
    {encoding: 'utf8', timeout: 10000});
  const preview = cli('pin-preview', filename, 'temporal_cpis=2', 'tracking=true');
  assert.equal(preview.status, 0, preview.stderr);
  fs.writeFileSync(contract, preview.stdout);
  assert.equal(cli('accept', filename, contract).status, 0);
  const wrong = JSON.parse(fs.readFileSync(filename, 'utf8'));
  wrong.process.detection.integration_cpis = 1;
  fs.writeFileSync(filename, JSON.stringify(wrong));
  assert.equal(cli('accept', filename, contract).status, 78);
}));

// These synthetic receipts test reporting/admission only, not radar numerical
// qualification. Production pins come from independently qualified native plans.
function runtimeFixture({input = 'live', backend = 'cpu', channelized = false, now = 10000} = {}) {
  const config = configured(4);
  if (channelized) config.process.multi_illuminator = {enabled: true, illuminators: [
    {enabled: true, id: 'fm', standard: 'FM', frequency_hz: config.capture.fc,
      output_sample_rate_hz: config.capture.fs}]};
  config.capture.replay.state = input === 'replay';
  const report = effectiveRadarPlan(config), lane = report.lanes[0];
  const native = JSON.stringify({schema: 'vectorwarp-resolved-products-v1', ...lane.configured_products});
  const hash = crypto.createHash('sha256').update(native).digest('hex');
  const requirement = {...pin(config), resolved_products: {[lane.configured_sha256]: hash}};
  Object.assign(report, requirement, {qualification: 'matched', errors: []});
  const lineage = {available: true, sampleInterval: 'half-open', captureEpochExact: '0',
    firstInputSampleExact: '9007199254740993', endInputSampleExact: '9007199254741993'};
  const execution = {...lineage, known: true, frameBound: true, provenance: 'processor_stage_receipt',
    cpuExecuted: backend === 'cpu', gpuExecuted: backend !== 'cpu'};
  const frame = {illuminator_id: lane.id, resolved_product_plan: native, input_lineage: lineage,
    product_stage_execution: Object.fromEntries(['fusion', 'temporal', 'detector', 'tracker']
      .map(stage => [stage, {backend, device: 'fixture-only', execution: {...execution}}]))};
  const observation = {schema: 'vectorwarp-runtime-plan-observation-v1', generation: 0,
    observed_at: now, input, processor_fresh: true,
    products: ['map', 'detection', 'tracker'].map(product =>
      ({lane_id: lane.id, product, generation: 0, received_at: now, frame: clone(frame)}))};
  return {config, report, requirement, observation, frame, hash, now};
}

test('native reporting contract is invariant across ingress, input and executing backend', () => {
  const hashes = new Set(), configuredHashes = new Set();
  for (const input of ['live', 'replay']) for (const channelized of [false, true])
    for (const backend of ['cpu', 'vulkan', 'vulkan-resident', 'vulkan-temporal']) {
      const fixture = runtimeFixture({input, channelized, backend});
      if (backend === 'vulkan-temporal') for (const product of fixture.observation.products)
        for (const stage of ['fusion', 'tracker']) {
          const receipt = product.frame.product_stage_execution[stage];
          receipt.backend = 'cpu'; receipt.device = '';
          Object.assign(receipt.execution, {cpuExecuted: true, gpuExecuted: false});
        }
      const result = assessResolvedRadarPlans(fixture.report, fixture.observation, {now: fixture.now});
      assert.equal(result.qualification, 'matched', result.errors.join('; '));
      hashes.add(result.lanes[0].native_sha256);
      configuredHashes.add(result.lanes[0].configured_sha256);
    }
  assert.equal(hashes.size, 1);
  assert.equal(configuredHashes.size, 1);
});

test('runtime acceptance rejects missing intent, stale generations and counterfeit capability receipts', () => {
  const mutations = [
    f => { delete f.report.resolved_products; },
    f => { f.report.qualification = 'unpinned'; },
    f => { f.observation.observed_at -= 5001; },
    f => { f.observation.processor_fresh = false; },
    f => { f.observation.input = 'unknown'; },
    f => { f.observation.products = {}; },
    f => { f.observation.products.pop(); },
    f => { f.observation.products.push(clone(f.observation.products[0])); },
    f => { f.observation.products[0].generation++; },
    f => { f.observation.products[0].received_at--; f.now += 5000; },
    f => { delete f.observation.products[0].frame.resolved_product_plan; },
    f => { f.observation.products[0].frame.resolved_product_plan = '{bad'; },
    f => { f.observation.products[0].frame.resolved_product_plan += ' '; },
    f => { f.observation.products[0].frame.input_lineage.available = false; },
    f => { f.observation.products[0].frame.input_lineage.firstInputSampleExact = 9007199254740993; },
    f => { f.observation.products[0].frame.input_lineage.endInputSampleExact = '18446744073709551616'; },
    f => { f.observation.products[0].frame.input_lineage.captureEpochExact = '1'; },
    f => { delete f.observation.products[0].frame.product_stage_execution.temporal; },
    f => { f.observation.products[0].frame.product_stage_execution.detector.execution.cpuExecuted = false; },
    f => { f.observation.products[0].frame.product_stage_execution.tracker.execution.frameBound = false; },
    f => { f.observation.products[0].frame.product_stage_execution.fusion.execution.endInputSampleExact = '99'; }
  ];
  mutations.forEach((mutate, index) => {
    const fixture = runtimeFixture(); mutate(fixture);
    assert.equal(assessResolvedRadarPlans(fixture.report, fixture.observation, {now: fixture.now}).qualification,
      'rejected', `mutation ${index}`);
  });
});

test('a self-pinned native downgrade still fails directly comparable configured behavior', () => {
  for (const [field, wrong] of [['integrationCpis', 1], ['stages.detection', false],
    ['trackerM', 1], ['trackerN', 2], ['trackerDelete', 1]]) {
    const fixture = runtimeFixture(), native = JSON.parse(fixture.frame.resolved_product_plan);
    native[field] = wrong;
    const serialized = JSON.stringify(native);
    fixture.report.resolved_products[fixture.report.lanes[0].configured_sha256] =
      crypto.createHash('sha256').update(serialized).digest('hex');
    for (const product of fixture.observation.products) product.frame.resolved_product_plan = serialized;
    assert.ok(assessResolvedRadarPlans(fixture.report, fixture.observation, {now: fixture.now}).errors
      .some(error => error.includes(`native ${field} differs`)));
  }
  const fixture = runtimeFixture();
  assert.ok(radarPlanErrors(fixture.report, {...fixture.requirement, resolved_products: {}}).length);
});

test('session-owned observations clear on restart and cannot be refreshed by old streams or aliases', () => {
  for (const input of ['live', 'replay']) for (const channelized of [false, true]) {
    const fixture = runtimeFixture({input, channelized});
    let now = fixture.now;
    const store = new RadarSessionStore({config: fixture.config, now: () => now});
    const connection = store.open(channelized ? 'illuminator' : 'map'); store.activate(connection);
    store.acceptProcessorStatus({valid: true, value: {input, state: input === 'live' ? 'live' : 'playing'}}, {});
    const map = {...fixture.frame, timestamp: now, delay: [1], doppler: [2], data: [[3]]};
    const detections = {...fixture.frame, timestamp: now, delay: [1], doppler: [2], snr: [3]};
    const tracks = {...fixture.frame, timestamp: now, data: []};
    const envelope = {schema: 'vectorwarp-multi-illuminator-frame-v1', id: 'fm', timestamp: now,
      frequency_hz: fixture.config.capture.fc, sample_rate_hz: fixture.config.capture.fs,
      first_input_sample: 0, end_input_sample: 999, map, detections, tracks};
    const publish = () => {
      if (channelized) {
        const admitted = store.acceptIlluminator(JSON.stringify(envelope), connection);
        store.publishPrimary(admitted.value, admitted.duplicate, admitted.map);
      } else for (const [name, frame] of [['map', map], ['detection', detections], ['tracker', tracks]])
        store.acceptProduct(name, JSON.stringify(frame), {connection, parsed: frame});
    };
    publish();
    assert.equal(assessResolvedRadarPlans(fixture.report, store.planObservation(), {now}).qualification, 'matched');
    now++;
    publish();
    assert.ok(store.planObservation().products.every(product => product.received_at === fixture.now),
      'Duplicate publication must not refresh an execution receipt');
    store.restart(); publish();
    assert.equal(store.planObservation().products.length, 0);
    assert.equal(store.planObservation().processor_fresh, false);
    assert.equal(assessResolvedRadarPlans(fixture.report, store.planObservation(), {now}).qualification, 'rejected');
  }
});

test('runtime CLI rechecks raw receipts against independent pins and exact loaded configuration', () => fixture(({filename, contract}) => {
  const fixture = runtimeFixture({now: Date.now()});
  fs.writeFileSync(filename, JSON.stringify(fixture.config));
  fs.writeFileSync(contract, JSON.stringify(fixture.requirement));
  const document = readConfig(filename), reportPath = `${filename}.runtime.json`;
  const runtime = {loadedRevision: document.revision, loaded: document.radarPlan,
    observation: fixture.observation, execution: {qualification: 'matched', errors: []}};
  const accept = () => {
    fs.writeFileSync(reportPath, JSON.stringify(runtime));
    return spawnSync(process.execPath, [path.join(__dirname, 'config-store.js'), 'accept-runtime',
      filename, contract, reportPath], {encoding: 'utf8', timeout: 10000});
  };
  let result = accept(); assert.equal(result.status, 0, result.stderr);
  runtime.observation.products.pop();
  result = accept(); assert.equal(result.status, 78, 'A precomputed acceptance boolean is never trusted');
  runtime.observation = fixture.observation; runtime.loadedRevision = 'wrong';
  result = accept(); assert.equal(result.status, 78); assert.match(result.stderr, /exact loaded configuration/);
}));
