'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const yaml = require('js-yaml');
const {FIELD_RULES, validateConfig, getDeviceProfiles, effectiveRadarPlan} = require('./config-manager');
const {readConfig, saveConfig, configurationForReceiver, setupDefaults} = require('./config-store');
// The canonical field contract must load without Node APIs and serialize to
// exactly the same browser projection; no generated checked-in schema copy.
const browserRules = {};
require('node:vm').runInNewContext(fs.readFileSync(path.join(__dirname, 'config-rules.js'), 'utf8'), browserRules);
assert.equal(JSON.stringify(browserRules.VectorWarpSettingRules(
  require('./adsb-discovery').SOURCE_CHOICES)), JSON.stringify(FIELD_RULES));
for (const [key, rule] of Object.entries(FIELD_RULES)) {
  if (rule.presentation) {
    assert.equal(rule.presentation.length, 2, `${key}: one label/help contract`);
    assert.ok(rule.presentation.every(value => typeof value === 'string'), key);
  }
  const control = rule.control;
  if (!control) continue;
  if (control.choices === true) assert.ok(Array.isArray(rule.choices), key);
  else if (typeof control.choices === 'string')
    assert.ok(['receivers', 'channels', 'surveillance'].includes(control.choices), key);
  else if (control.choices) assert.ok(Array.isArray(control.choices), key);
  if (control.effect) assert.ok(['receiver', 'channels', 'reference'].includes(control.effect), key);
  if (control.scale !== undefined) assert.ok(Number.isFinite(control.scale) && control.scale > 0, key);
}
const clone = value => JSON.parse(JSON.stringify(value));
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-settings-matrix-'));
function leaves(value, prefix = []) { return Array.isArray(value) || value === null || typeof value !== 'object' ? [{path: prefix.join('.')}]
  : Object.entries(value).flatMap(([key, child]) => leaves(child, [...prefix, key])); }
function receiverConfig(name) { const value = name === 'config.yml' ? setupDefaults() : configurationForReceiver(name);
  if (value.capture.device.type === 'HackRF') value.capture.device.serial = ['0000000000000001', '0000000000000002']; return value; }
function get(value, key) { return key.split('.').reduce((item, name) => item?.[name], value); }
function put(value, key, replacement) { const names = key.split('.'); const parent = names.slice(0, -1).reduce((item, name) => item[name], value); parent[names.at(-1)] = replacement; }
function invalidFor(rule) { return rule.type === 'boolean' ? 'false' : rule.type === 'number' || rule.type === 'gain' ? 'invalid' : rule.type === 'array' ? 'invalid' : 7; }
function validDifferent(key, current, rule) {
  const explicit = {
    'capture.fs': 2000001, 'capture.fc': 204640001, 'capture.device.heimdall.host': '127.0.0.2',
    'capture.aircraft_clips.maximum_slant_range_km': 999,
    'capture.aircraft_clips.maximum_elevation_deg': 89,
    'capture.device.heimdall.port': 8093, 'capture.device.heimdall.control_port': 8094,
    'capture.device.heimdall.gain': -1, 'capture.device.heimdall.gain_control': 'fixed',
    'capture.device.address': 'type=b200,serial=0000000000000001',
    'capture.device.subdev': 'A:B A:A', 'capture.device.serial': ['0000000000000003','0000000000000004'],
    'capture.device.antenna': ['TX/RX','TX/RX'], 'capture.device.gain': [21,21],
    'capture.device.gain_lna': [16,16], 'capture.device.gain_vga': [20,20], 'capture.device.gainReduction': [51,51],
    'capture.device.amp_enable': [true,true], 'capture.replay.file': '/tmp/changed.blah2iq',
    'capture.replay.legacy_block_samples': 4096, 'network.ip': '127.0.0.1',
    'truth.adsb.tar1090': 'auto', 'save.path': '/tmp/vectorwarp/', 'location.rx.name': 'Changed receiver', 'location.tx.name': 'Changed transmitter',
    'process.reference_synthesis.channels': undefined
  };
  if (explicit[key] !== undefined) return explicit[key];
  if (rule.choices) return rule.choices.find(value => JSON.stringify(value) !== JSON.stringify(current));
  if (rule.type === 'boolean') return !current;
  if (rule.type === 'number') return typeof current === 'number' ? current + (Number.isInteger(current) ? 1 : .01) : 1;
  if (rule.type === 'string') return `${current}-changed`;
  return undefined;
}
try {
  for (const type of ['Kraken', 'RspDuo', 'Usrp', 'HackRF']) {
    const source = receiverConfig(type), plan = effectiveRadarPlan(source).semantic_sha256;
    const filename = path.join(directory, `display-${type}.yml`);
    fs.writeFileSync(filename, yaml.dump(source));
    assert.equal(source.display.max_hold_decay_db_per_second, 1);
    for (const rate of [0, .001, 20]) {
      const candidate = clone(source); candidate.display.max_hold_decay_db_per_second = rate;
      assert.equal(validateConfig(candidate, source).valid, true, `${type}: display rate ${rate}`);
      saveConfig(filename, candidate, readConfig(filename).revision);
      assert.deepEqual(readConfig(filename).config, candidate, `${type}: display round trip`);
      assert.equal(effectiveRadarPlan(candidate).semantic_sha256, plan, 'Display cannot change radar math');
    }
    delete source.display;
    fs.writeFileSync(filename, yaml.dump(source));
    assert.equal(readConfig(filename).config.display.max_hold_decay_db_per_second, 1,
      'Existing installations receive the same canonical display default');
  }
  // Upgrade-only flags are covered by tracker-publication.test.js retirement tests.
  const covered = new Set();
  for (const name of ['HackRF', 'Usrp', 'Kraken']) {
    const source = receiverConfig(name); const validation = validateConfig(source);
    assert.equal(validation.valid, true, `${name}: ${validation.errors.join('; ')}`);
    const filename = path.join(directory, name); fs.writeFileSync(filename, yaml.dump(source));
    const saved = saveConfig(filename, source, readConfig(filename).revision);
    assert.deepEqual(readConfig(filename).config, saved.config, `${name} failed save/load round trip`);
    for (const leaf of leaves(saved.config)) if (FIELD_RULES[leaf.path]) covered.add(leaf.path);
  }
  const rspSource = configurationForReceiver('RspDuo', receiverConfig('config.yml'));
  { const filename = path.join(directory, 'generated-rspduo.yml');
    const validation = validateConfig(rspSource);
    assert.equal(validation.valid, true, `generated RSPduo: ${validation.errors.join('; ')}`);
    fs.writeFileSync(filename, yaml.dump(rspSource));
    const saved = saveConfig(filename, rspSource, readConfig(filename).revision);
    assert.deepEqual(readConfig(filename).config, saved.config, 'generated RSPduo failed save/load round trip');
    for (const leaf of leaves(saved.config)) if (FIELD_RULES[leaf.path]) covered.add(leaf.path); }
  const base = receiverConfig('Kraken');
  for (const profile of getDeviceProfiles()) {
    const candidate = clone(base); candidate.capture.fs = profile.sampleRate; candidate.capture.device = clone(profile.device);
    if (profile.type === 'HackRF') candidate.capture.device.serial = ['0000000000000001', '0000000000000002'];
    if (!profile.process?.reference_synthesis) delete candidate.process.reference_synthesis;
    for (const [key, value] of Object.entries(profile.process || {})) candidate.process[key] = clone(value);
    const result = validateConfig(candidate, base); assert.equal(result.valid, true, `${profile.type}: ${result.errors.join('; ')}`);
    for (const leaf of leaves(candidate)) if (FIELD_RULES[leaf.path]) covered.add(leaf.path);
  }
  const missing = Object.keys(FIELD_RULES).filter(key => !covered.has(key) && !FIELD_RULES[key].optional && !FIELD_RULES[key].readOnly);
  assert.deepEqual(missing, [], `schema fields missing from shipped/profile matrix: ${missing.join(', ')}`);
  // Exercise scalar validation against shipped documents. HTTP rejection and
  // unchanged-file/revision guarantees are covered by config-server.test.js;
  // config-store intentionally expects its caller to validate before writing.
  let rejected = 0;
  let changed = 0;
  const edited = new Set();
  // Prefer Kraken first while still validating portable fields across every
  // shipped receiver profile.
  const sources = [...['Kraken', 'HackRF', 'Usrp'].map(receiverConfig), rspSource];
  for (const [key, rule] of Object.entries(FIELD_RULES)) {
    if (rule.readOnly) continue;
    const source = sources.find(value => get(value, key) !== undefined);
    if (!source) continue; // Absent legacy default; config-recovery supplies it.
    const replacement = validDifferent(key, get(source, key), rule);
    if (replacement !== undefined) {
      const filename = path.join(directory, `changed-${changed}.yml`); fs.writeFileSync(filename, yaml.dump(source));
      const baseline = readConfig(filename); const candidate = clone(baseline.config); put(candidate, key, replacement);
      const checked = validateConfig(candidate, source);
      if (checked.valid) {
        assert.notDeepEqual(get(source, key), replacement, `${key} mutation must differ from baseline`);
        const saved = saveConfig(filename, candidate, baseline.revision);
        assert.deepEqual(readConfig(filename).config, candidate, `${key} save/load changed unrelated settings`);
        assert.deepEqual(get(readConfig(filename).config, key), replacement, `${key} valid edit did not persist`);
        assert.deepEqual(get(saved.config, key), replacement); changed++; edited.add(key);
      }
    }
    const invalidValues = [invalidFor(rule)];
    if (rule.type === 'number') {
      invalidValues.push(null, false, [], {}, NaN, Infinity);
      if (rule.min !== undefined) invalidValues.push(rule.min - 1);
      if (rule.max !== undefined) invalidValues.push(rule.max + 1);
      if (rule.integer) invalidValues.push(1.5);
    }
    for (const invalid of invalidValues) {
      const candidate = clone(source); put(candidate, key, invalid);
      const result = validateConfig(candidate, source);
      assert.equal(result.valid, false, `${key} invalid value ${String(invalid)} was accepted`);
      assert.ok(result.errors.some(error => error.startsWith(key)), `${key} needs a field-specific diagnostic`);
      rejected++;
    }
  }
  // Optional USRP-block replay setting is a real persisted edit with a
  // cross-field dependency, rather than merely a schema inventory entry.
  const legacyBase = receiverConfig('Usrp'); const legacyFile = path.join(directory, 'legacy-block.yml'); fs.writeFileSync(legacyFile, yaml.dump(legacyBase));
  const legacy = clone(readConfig(legacyFile).config); legacy.capture.replay = {...legacy.capture.replay, state: true, format: 'usrp-blocks', legacy_block_samples: 4096};
  assert.equal(validateConfig(legacy).valid, true);
  const invalidLegacy = clone(legacy); invalidLegacy.capture.replay.legacy_block_samples = 0;
  assert.equal(validateConfig(invalidLegacy).valid, false);
  rejected++;
  { const filename = legacyFile;
    saveConfig(filename, legacy, readConfig(filename).revision);
    assert.deepEqual(readConfig(filename).config, legacy, 'legacy block setting did not persist'); }
  edited.add('capture.replay.legacy_block_samples');
  // Cross-field settings need coherent groups, rather than scalar mutation.
  const persistGroup = (name, baseline, candidate, keys) => {
    const result = validateConfig(candidate); assert.equal(result.valid, true, `${name}: ${result.errors.join('; ')}`);
    const filename = path.join(directory, `${name}.yml`); fs.writeFileSync(filename, yaml.dump(baseline));
    const saved = saveConfig(filename, candidate, readConfig(filename).revision);
    assert.deepEqual(readConfig(filename).config, candidate, `${name} save/load changed unrelated settings`);
    for (const key of keys) { assert.notDeepEqual(get(baseline, key), get(candidate, key), `${name} did not change ${key}`); assert.deepEqual(get(saved.config, key), get(candidate, key), `${name} did not persist ${key}`); edited.add(key); }
  };
  const replayBase = receiverConfig('Usrp');
  replayBase.process.performance = {surveillance_workers: 1, fft_threads: 4, acceleration: 'auto'};
  const mapOnly = clone(replayBase);
  mapOnly.process.detection.enable = false;
  mapOnly.process.tracker.enable = false;
  persistGroup('map-without-detector-or-tracker', replayBase, mapOnly,
    ['process.detection.enable', 'process.tracker.enable']);
  const portableReplay = clone(replayBase);
  Object.assign(portableReplay.capture.replay, {state: true,
    sample_rate_hz: 2400000, frequency_hz: 509000000, channel_count: 5,
    reference_channel: 0, reference_mode: 'array_eigenbeam',
    surveillance_channels: [0,1,2,3,4], reference_channels: [0,1,2,3,4]});
  persistGroup('portable-replay-profile', replayBase, portableReplay, [
    'capture.replay.sample_rate_hz', 'capture.replay.frequency_hz',
    'capture.replay.channel_count', 'capture.replay.reference_channel',
    'capture.replay.reference_mode', 'capture.replay.surveillance_channels',
    'capture.replay.reference_channels']);
  const krakenBase = receiverConfig('Kraken'); const kraken = clone(krakenBase);
  kraken.capture.device.channel_count = 6; kraken.capture.device.reference_channel = 1;
  kraken.capture.device.surveillance_channels = [0,2,3,4,5];
  kraken.process.reference_synthesis = {...kraken.process.reference_synthesis, mode: 'dedicated', channels: [1]};
  kraken.process.performance = {...kraken.process.performance, acceleration: 'cpu', surveillance_workers: 2, fft_threads: 2};
  Object.assign(kraken.network.ports, {api: 3100, map: 3101, detection: 3102, track: 3103, timestamp: 3104, timing: 3105, iqdata: 3106});
  persistGroup('kraken-coherent-edit', krakenBase, kraken, ['capture.device.channel_count','capture.device.reference_channel','capture.device.surveillance_channels','process.reference_synthesis.mode','process.reference_synthesis.channels','process.performance.acceleration','process.performance.surveillance_workers','process.performance.fft_threads','network.ports.api','network.ports.map','network.ports.detection','network.ports.track','network.ports.timestamp','network.ports.timing','network.ports.iqdata']);
  const channelGains = clone(krakenBase);
  channelGains.capture.device.heimdall.gain = 18;
  channelGains.capture.device.heimdall.gain_control = 'fixed';
  channelGains.capture.device.heimdall.channel_gains = [19.7, 25.4, 22.9, 25.4, 22.9];
  persistGroup('kraken-channel-gains', krakenBase, channelGains,
    ['capture.device.heimdall.gain', 'capture.device.heimdall.gain_control',
      'capture.device.heimdall.channel_gains']);
  const badChannelGains = clone(channelGains);
  badChannelGains.capture.device.heimdall.gain_control = 'measured';
  assert.equal(validateConfig(badChannelGains).valid, false,
    'measured common gain must not silently overwrite coherent channel gains');
  const automaticChannelGains = clone(channelGains);
  automaticChannelGains.capture.device.heimdall.gain_control = 'measured_per_channel';
  assert.equal(validateConfig(automaticChannelGains).valid, true,
    'automatic per-channel gain accepts a verified starting vector');
  const fusion = clone(krakenBase);
  fusion.process.performance.acceleration = 'gpu-resident';
  fusion.process.tracker.gpu_capacity = 128;
  fusion.process.surveillance_fusion.mode = 'coherent_beam_bank';
  fusion.process.surveillance_fusion.beams = 6;
  fusion.process.surveillance_fusion.effective_beam_trials = 6;
  fusion.process.detection.integration_cpis = 2;
  fusion.process.detection.integration_motion_bins = 1;
  fusion.process.detection.track_before_detect = {...fusion.process.detection.track_before_detect,
    enable: true, max_cpis: 4, motion_bins: 0,
    delay_motion_bins: 2, doppler_motion_bins: 16};
  fusion.process.tracker.initiate.N = 1 + 4 * (fusion.process.tracker.initiate.M - 1);
  fusion.process.tracker.beam_tolerance = 1;
  fusion.capture.device.array_geometry = {
    shape: 'regular_polygon', units: 'm', coordinate_frame: 'array_xy_right_handed',
    mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true,
    x_axis_bearing_deg_true: 0,
    bearing_channels: [...fusion.capture.device.surveillance_channels],
    elements: fusion.capture.device.surveillance_channels.map((daq_channel, index) => ({
      id: `channel-${daq_channel}`, daq_channel, receiver_serial: String(1000 + daq_channel),
      position: [Math.cos(2 * Math.PI * index / 5) * .2,
        Math.sin(2 * Math.PI * index / 5) * .2, 0]
    }))
  };
  // Confirmation flags are meaningful persisted edits only when the measured
  // channel geometry itself is unchanged from the reviewed baseline.
  krakenBase.capture.device.array_geometry = clone(
    fusion.capture.device.array_geometry);
  krakenBase.capture.device.array_geometry.mapping_confirmed = false;
  krakenBase.capture.device.array_geometry.geometry_confirmed = false;
  krakenBase.capture.device.array_geometry.orientation_confirmed = false;
  persistGroup('kraken-coherent-fusion', krakenBase, fusion,
    ['capture.device.array_geometry.mapping_confirmed',
      'capture.device.array_geometry.geometry_confirmed',
      'capture.device.array_geometry.orientation_confirmed',
      'process.performance.acceleration', 'process.tracker.gpu_capacity',
      'process.surveillance_fusion.mode',
      'process.surveillance_fusion.beams',
      'process.surveillance_fusion.effective_beam_trials',
      'process.detection.integration_cpis',
      'process.detection.integration_motion_bins',
      'process.detection.track_before_detect.enable',
      'process.detection.track_before_detect.max_cpis',
      'process.detection.track_before_detect.motion_bins',
      'process.detection.track_before_detect.delay_motion_bins',
      'process.detection.track_before_detect.doppler_motion_bins',
      'process.tracker.initiate.N',
      'process.tracker.beam_tolerance']);
  const fractionalFusion = clone(fusion);
  fractionalFusion.process.detection.track_before_detect.fractional_delay = true;
  persistGroup('resident-fractional-delay', fusion, fractionalFusion,
    ['process.detection.track_before_detect.fractional_delay']);
  const measuredBaseline = clone(fusion);
  Object.assign(measuredBaseline.capture.device.array_geometry,
    {mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true});
  measuredBaseline.capture.device.heimdall.gain = 14;
  measuredBaseline.capture.device.heimdall.gain_control = 'fixed';
  const measuredFusion = clone(measuredBaseline);
  measuredFusion.process.performance.acceleration = 'cpu';
  delete measuredFusion.process.tracker.beam_tolerance;
  measuredFusion.process.detection.integration_cpis = 1;
  measuredFusion.process.detection.integration_motion_bins = 0;
  measuredFusion.process.detection.track_before_detect = {...measuredFusion.process.detection.track_before_detect,
    enable: false, max_cpis: 8, motion_bins: 1};
  measuredFusion.process.surveillance_fusion.manifold_file =
    '/etc/vectorwarp/detection-manifold.json';
  persistGroup('kraken-measured-fusion', measuredBaseline, measuredFusion,
    ['process.surveillance_fusion.manifold_file']);
  const bearingBaseline = clone(measuredBaseline);
  const bearing = clone(bearingBaseline);
  bearing.process.bearing.enable = true;
  bearing.process.bearing.method = 'covariance_mvdr';
  bearing.process.bearing.reference_anchor = 'fixed_site';
  bearing.process.bearing.scan_beams = 60;
  bearing.process.bearing.guard_radius = 2;
  bearing.process.bearing.training_radius = 4;
  bearing.process.bearing.diagonal_loading = .02;
  bearing.process.bearing.max_rate_deg_s = 30;
  bearing.process.bearing.manifold_file = '/etc/vectorwarp/bearing-manifold.json';
  bearing.process.bearing.channel_mapping = [1, 0, 2, 3, 4];
  bearing.process.bearing.manifold_recorder.enabled = true;
  bearing.truth.adsb.enabled = true;
  bearing.network.ports.bearing = 3014;
  persistGroup('kraken-bearing-output', bearingBaseline, bearing,
    ['process.bearing.enable', 'process.bearing.method', 'process.bearing.reference_anchor',
      'process.bearing.scan_beams',
      'process.bearing.guard_radius', 'process.bearing.training_radius',
      'process.bearing.diagonal_loading', 'process.bearing.max_rate_deg_s',
      'process.bearing.manifold_file', 'process.bearing.channel_mapping',
      'process.bearing.manifold_recorder.enabled', 'truth.adsb.enabled',
      'network.ports.bearing']);
  const trackerForward = clone(krakenBase);
  trackerForward.process.performance.acceleration = 'auto';
  trackerForward.network.tracker_forward = {
    enabled: true, host: '127.0.0.2', port: 30101, annotate_adsb: false,
    unassisted: {enabled: true, host: '127.0.0.3', port: 30102}
  };
  trackerForward.truth.adsb.delay_tolerance = 3;
  trackerForward.truth.adsb.doppler_tolerance = 6;
  persistGroup('tracker-forward-output', krakenBase, trackerForward,
    ['network.tracker_forward.enabled', 'network.tracker_forward.host',
      'network.tracker_forward.port',
      'network.tracker_forward.annotate_adsb',
      'network.tracker_forward.unassisted.enabled',
      'network.tracker_forward.unassisted.host',
      'network.tracker_forward.unassisted.port', 'truth.adsb.delay_tolerance',
      'truth.adsb.doppler_tolerance']);
  const cpuRecorder = clone(fusion);
  Object.assign(cpuRecorder.capture.device.array_geometry, {
    mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true
  });
  cpuRecorder.process.performance.acceleration = 'cpu';
  cpuRecorder.process.detection.integration_cpis = 1;
  cpuRecorder.process.detection.integration_motion_bins = 0;
  cpuRecorder.process.detection.track_before_detect.enable = false;
  delete cpuRecorder.process.tracker.beam_tolerance;
  cpuRecorder.process.bearing.manifold_recorder.enabled = true;
  cpuRecorder.truth.adsb.enabled = true;
  assert.equal(validateConfig(cpuRecorder).valid, true,
    'CPU coherent Kraken installations must be able to record manifold samples');
  const targetBase = clone(cpuRecorder);
  targetBase.capture.replay.state = true;
  const targetProtection = clone(targetBase);
  targetProtection.process.bearing.manifold_recorder.enabled = false;
  targetProtection.process.reference_synthesis.target_protection_enabled = true;
  targetProtection.save.complex_cutout = {...targetProtection.save.complex_cutout,
    state: true, path: '/tmp/experimental-cutout-new.jsonl'};
  persistGroup('experimental-target-reference-and-cutout', targetBase, targetProtection,
    ['process.reference_synthesis.target_protection_enabled', 'save.complex_cutout.state']);
  const liveProtection = clone(targetProtection);
  liveProtection.capture.replay.state = false;
  assert.equal(validateConfig(liveProtection).valid, false, 'Complex export remains replay-only');
  liveProtection.save.complex_cutout.state = false;
  const liveProtectionBase = clone(liveProtection);
  liveProtectionBase.process.reference_synthesis.target_protection_enabled = false;
  persistGroup('experimental-target-reference-live', liveProtectionBase, liveProtection,
    ['process.reference_synthesis.target_protection_enabled']);
  const rspBase = receiverConfig('Kraken'); const rsp = clone(rspBase); const rspProfile = getDeviceProfiles().find(profile => profile.type === 'RspDuo');
  rsp.capture.fs = rspProfile.sampleRate; rsp.capture.device = clone(rspProfile.device); rsp.capture.device.serial = '0000000000000001';
  delete rsp.process.reference_synthesis; rsp.process.performance = {...clone(rspProfile.process.performance), acceleration: 'auto'};
  persistGroup('rsp-profile-edit', rspBase, rsp, ['capture.fs','capture.device.type','capture.device.serial']);
  // Explicit resident capacity is optional in legacy files but must persist
  // as a coherent resident-mode edit without changing unrelated settings.
  const residentBase = clone(fusion);
  residentBase.process.tracker.initiate.acceleration_model = 'hypothesis_bank';
  const resident = clone(residentBase);
  const conservativeTracker = clone(residentBase);
  conservativeTracker.process.tracker.profile = 'conservative';
  conservativeTracker.process.tracker.initiate.min_snr_db = 7;
  persistGroup('conservative-tracker-profile', residentBase, conservativeTracker, [
    'process.tracker.profile', 'process.tracker.initiate.min_snr_db']);
  resident.process.tracker.initiate.acceleration_model = 'adaptive_gate';
  resident.process.tracker.initiate.stationary_clutter = {enable: true,
    persistence_cpis: 8, max_doppler_hz: 15};
  persistGroup('resident-capacity-edit', residentBase, resident, [
    'process.tracker.initiate.acceleration_model',
    'process.tracker.initiate.stationary_clutter.enable',
    'process.tracker.initiate.stationary_clutter.persistence_cpis',
    'process.tracker.initiate.stationary_clutter.max_doppler_hz']);
  for (const mode of [undefined, 'cpu', 'auto', 'gpu']) {
    const portable = clone(resident);
    if (mode === undefined) delete portable.process.performance;
    else portable.process.performance.acceleration = mode;
    portable.process.bearing.enable = false;
    delete portable.process.tracker.beam_tolerance;
    delete portable.process.tracker.gpu_capacity;
    const result = validateConfig(portable);
    assert.equal(result.valid, true,
      `Shared stationary recovery accepts ${mode ?? 'implicit AUTO'}: ${result.errors.join('; ')}`);
  }
  const legacyNoPerformance = clone(krakenBase);
  delete legacyNoPerformance.process.performance;
  legacyNoPerformance.process.tracker.initiate.stationary_clutter = {enable: false};
  assert.equal(validateConfig(legacyNoPerformance).valid, true,
    `disabled resident features remain valid with implicit AUTO: ${validateConfig(legacyNoPerformance).errors.join('; ')}`);
  // Snapshot controls are tested within an existing replay context. Switching
  // from live capture to replay intentionally invalidates geometry confirmation.
  const residentSnapshotBase = clone(residentBase);
  residentSnapshotBase.capture.replay.state = true;
  const residentReplaySnapshot = clone(residentSnapshotBase);
  residentReplaySnapshot.capture.replay.resident_snapshot = {enabled: true,
    output_directory: '/var/tmp/vectorwarp-resident-snapshots', max_frames: 20,
    max_bytes: 268435456, source_sha256: 'a'.repeat(64),
    rf_profile_sha256: 'b'.repeat(64), rf_profile_file: '/var/tmp/replay.json'};
  persistGroup('resident-replay-snapshot', residentSnapshotBase, residentReplaySnapshot, [
    'capture.replay.resident_snapshot.enabled',
    'capture.replay.resident_snapshot.output_directory',
    'capture.replay.resident_snapshot.max_frames',
    'capture.replay.resident_snapshot.max_bytes',
    'capture.replay.resident_snapshot.source_sha256',
    'capture.replay.resident_snapshot.rf_profile_sha256',
    'capture.replay.resident_snapshot.rf_profile_file']);
  const measuredGain = clone(krakenBase);
  measuredGain.process.performance.acceleration = 'auto';
  measuredGain.capture.device.heimdall.gain_control = 'measured';
  persistGroup('measured-gain-policy', krakenBase, measuredGain, ['capture.device.heimdall.gain_control']);
  const adsbBaseline = clone(krakenBase);
  adsbBaseline.process.performance.acceleration = 'auto';
  const adsbSources = clone(adsbBaseline);
  adsbSources.truth.adsb.sources = ['local:readsb', 'https://backup.example/tar1090'];
  persistGroup('structured-adsb-sources', adsbBaseline, adsbSources, ['truth.adsb.sources']);
  const uncertaintyAssociation = clone(adsbBaseline);
  uncertaintyAssociation.truth.adsb.uncertainty_association = {enable: true,
    max_projection_s: 3, acceleration_sigma_m_s2: 2,
    velocity_sigma_m_s: 3, vertical_position_sigma_m: 45,
    vertical_velocity_sigma_m_s: 3, max_area_bins: 128};
  persistGroup('truth-uncertainty-association', adsbBaseline,
    uncertaintyAssociation, Object.keys(uncertaintyAssociation.truth.adsb.uncertainty_association)
      .map(key => `truth.adsb.uncertainty_association.${key}`));
  const remoteIdSource = clone(adsbBaseline);
  remoteIdSource.truth.remote_id.source = 'http://remote-id.local:8754';
  persistGroup('remote-id-source', adsbBaseline, remoteIdSource, ['truth.remote_id.source']);
  const multiIlluminatorBase = clone(krakenBase);
  multiIlluminatorBase.process.performance.acceleration = 'auto';
  const multiIlluminator = clone(multiIlluminatorBase);
  multiIlluminator.capture.fc = 90900000;
  multiIlluminator.process.multi_illuminator = {
    ...multiIlluminator.process.multi_illuminator, enabled: true, illuminators: [
    {id: 'fm-90.5', enabled: true, standard: 'FM', frequency_hz: 90500000,
      occupied_bandwidth_hz: 200000, output_sample_rate_hz: 300000,
      filter_taps_per_phase: 24,
      transmitter: {name: 'FM 90.5', latitude: 40.44, longitude: -80, altitude: 300}},
    {id: 'fm-91.3', enabled: true, standard: 'FM', frequency_hz: 91300000,
      occupied_bandwidth_hz: 200000, output_sample_rate_hz: 300000,
      filter_taps_per_phase: 24, tracker_min_snr_db: 5.5,
      transmitter: {name: 'FM 91.3', latitude: 40.45, longitude: -80.01, altitude: 310}}
  ]};
  // The scalar +1 probe collides with the causal-funnel port (3008).
  // Exercise a distinct output port with its multi-illuminator consumer enabled.
  multiIlluminator.network.ports.illuminator = 3117;
  persistGroup('multi-illuminator', multiIlluminatorBase, multiIlluminator,
    ['process.multi_illuminator.enabled', 'process.multi_illuminator.illuminators',
      'network.ports.illuminator']);
  for (const receiver of ['Kraken', 'Usrp', 'RspDuo', 'HackRF'])
    for (const stage of ['clutter', 'detection', 'tracker']) {
      const candidate = configurationForReceiver(receiver);
      candidate.capture.fc = multiIlluminator.capture.fc;
      if (receiver === 'HackRF') candidate.capture.device.serial = ['0001', '0002'];
      candidate.process.multi_illuminator = clone(multiIlluminator.process.multi_illuminator);
      if (receiver !== 'Kraken')
        candidate.process.multi_illuminator.illuminators.forEach(lane => { lane.output_sample_rate_hz = 250000; });
      candidate.process[stage].enable = false;
      const result = validateConfig(candidate);
      assert.equal(result.valid, stage !== 'detection', `${receiver}/${stage}: ${result.errors.join('; ')}`);
      if (stage === 'detection')
        assert(result.errors.includes('tracking requires process.detection.enable'));
      assert.equal(candidate.process[stage].enable, false, 'Validation must preserve saved values');
    }
  const fmReferenceReplay = clone(multiIlluminator);
  fmReferenceReplay.capture.replay.state = true;
  Object.assign(fmReferenceReplay.capture.replay,
    {sample_rate_hz: 2400000, frequency_hz: 90900000});
  fmReferenceReplay.process.multi_illuminator.experimental_fm_reference_quality_filter = true;
  persistGroup('experimental-fm-reference-replay', multiIlluminator, fmReferenceReplay,
    ['process.multi_illuminator.experimental_fm_reference_quality_filter']);
  const fmReferenceLive = clone(fmReferenceReplay); fmReferenceLive.capture.replay.state = false;
  persistGroup('experimental-fm-reference-live', multiIlluminator, fmReferenceLive,
    ['process.multi_illuminator.experimental_fm_reference_quality_filter']);
  const apiCausalBase = clone(krakenBase);
  apiCausalBase.process.performance.acceleration = 'auto';
  const apiCausal = clone(apiCausalBase);
  apiCausal.process.evidence.causal_funnel.api_ingest_enabled = true;
  persistGroup('api-ingested-causal-funnel', apiCausalBase, apiCausal,
    ['process.evidence.causal_funnel.api_ingest_enabled']);
  const replayPins = clone(apiCausalBase);
  Object.assign(replayPins.capture.replay, {recording_sha256: 'a'.repeat(64), acquisition_sha256: 'b'.repeat(64)});
  persistGroup('replay-acquisition-trust-pins', apiCausalBase, replayPins,
    ['capture.replay.recording_sha256', 'capture.replay.acquisition_sha256']);
  for (const malformed of [null, false, '', 'A'.repeat(64), 'a'.repeat(63)]) {
    const invalid = clone(replayPins); invalid.capture.replay.recording_sha256 = malformed;
    assert.equal(validateConfig(invalid).valid, false);
  }
  const missingIqPin = clone(replayPins); delete missingIqPin.capture.replay.recording_sha256;
  assert.equal(validateConfig(missingIqPin).valid, false);
  const remaining = Object.keys(FIELD_RULES).filter(key => !FIELD_RULES[key].readOnly && !edited.has(key));
  assert.deepEqual(remaining, [], `editable fields lack a persisted valid non-default case: ${remaining.join(', ')}`);
  console.log(`Configuration inventory covered ${covered.size}/${Object.keys(FIELD_RULES).length}; validated ${rejected} invalid values; persisted non-default edits for all ${edited.size} editable fields (${changed} scalar plus grouped cases).`);
} finally { fs.rmSync(directory, {recursive: true, force: true}); }
