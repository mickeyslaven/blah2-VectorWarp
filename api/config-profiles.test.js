'use strict';
const assert = require('assert');
const profiles = require('../html/js/config_profiles');
const geometry = require('../html/js/kraken_geometry');
const {validateConfig, getDeviceProfiles} = require('./config-manager');
const {processingControlCapabilities} = require('../html/js/config_ui');

const {configurationForReceiver, setupDefaults} = require('./config-store');
const source = configurationForReceiver('Kraken');
const clone = value => JSON.parse(JSON.stringify(value));
const configured = () => {
  const config = clone(source);
  config.capture.device.channel_count = 5;
  geometry.applyPreset(config, 'pentagon5');
  config.capture.device.array_geometry.x_axis_bearing_deg_true = 0;
  Object.assign(config.capture.device.array_geometry, {
    mapping_confirmed:true, geometry_confirmed:true, orientation_confirmed:true});
  config.process.performance = {...config.process.performance, acceleration: 'gpu-resident'};
  config.process.tracker.gpu_capacity = 64;
  config.process.tracker.initiate.stationary_clutter = {enable: true, persistence_cpis: 12, max_doppler_hz: 0};
  return config;
};
const qualifyTemporalFixture = config => {
  assert.ok(profiles.temporalConfirmationErrors(config).length,
    'selecting a deep preset must not silently widen confirmation');
  const depth = config.process.detection.track_before_detect.max_cpis ?? 8;
  config.process.tracker.initiate.N = 1 + depth * (config.process.tracker.initiate.M - 1);
  config.process.tracker.delete = Math.max(config.process.tracker.delete, depth - 1);
};

for (const profile of Object.keys(profiles.trackingProfiles)) {
  const policies = ['cpu', 'gpu', 'auto', 'gpu-resident'].map(acceleration => {
    const config = configurationForReceiver('Usrp');
    config.process.performance.acceleration = acceleration;
    profiles.applyTrackingProfile(config, profile);
    return {initiate: config.process.tracker.initiate, delete: config.process.tracker.delete,
      pfa: config.process.detection.pfa, minDoppler: config.process.detection.minDoppler};
  });
  for (const policy of policies) assert.deepEqual(policy, policies[0],
    `${profile} must select one tracker/detector policy across every backend`);
  if (profile === 'conservative') assert.equal(policies[0].initiate.acceleration_model, 'hypothesis_bank');
}

for (const type of ['Kraken', 'Usrp', 'RspDuo', 'HackRF'])
  for (const acceleration of ['cpu', 'gpu', 'auto', 'gpu-resident'])
    for (const depth of [4, 8]) {
      const config = configurationForReceiver(type);
      if (type === 'HackRF') config.capture.device.serial = ['0001', '0002'];
      config.process.performance.acceleration = acceleration;
      profiles.applyWeakTargetMode(config, depth === 4 ? 'four' : 'eight');
      Object.assign(config.process.tracker.initiate, {M: 4, N: 1 + depth * 3});
      config.process.tracker.delete = depth - 1;
      const backend = {stages: {resident: {selectable: acceleration === 'gpu-resident'}}};
      const check = expected => {
        const available = processingControlCapabilities(config, backend);
        const admission = validateConfig(config);
        assert.equal(admission.valid, expected, admission.errors.join('; '));
      };
      check(true);
      config.process.tracker.initiate.N--;
      check(false);
      config.process.tracker.initiate.N++;
      config.process.tracker.delete--;
      check(false);
    }

for (const filename of ['config.yml', 'HackRF', 'Kraken',
  'RspDuo', 'Usrp']) {
  const shipped = filename === 'config.yml' ? setupDefaults() : configurationForReceiver(filename);
  assert.equal(shipped.process.tracker.profile, 'balanced', `${filename} tracker profile`);
  assert.deepEqual([shipped.process.tracker.initiate.M, shipped.process.tracker.initiate.N],
    [3, 6], `${filename} balanced confirmation`);
}

{
  const config = configured();
  profiles.applyGainMode(config, 'automatic');
  assert.equal(config.capture.device.heimdall.gain_control, 'measured_per_channel');
  profiles.applyGainMode(config, 'manual');
  assert.equal(config.capture.device.heimdall.gain_control, 'fixed');
}
{
  const config = configured();
  profiles.applyClutterMode(config, 'automatic');
  assert.deepEqual([config.process.clutter.enable, config.process.clutter.followAmbiguity], [true, true]);
  profiles.applyClutterMode(config, 'off'); assert.equal(config.process.clutter.enable, false);
  profiles.applyClutterMode(config, 'custom');
  assert.deepEqual([config.process.clutter.enable, config.process.clutter.followAmbiguity], [true, false]);
  config.process.clutter.delayMin = config.process.ambiguity.delayMin;
  config.process.clutter.delayMax = config.process.ambiguity.delayMax;
  assert.equal(profiles.clutterMode(config), 'custom', 'legacy half-open bounds remain explicit custom intent');
  assert.deepEqual([config.process.clutter.delayMin, config.process.clutter.delayMax],
    [config.process.ambiguity.delayMin, config.process.ambiguity.delayMax]);
}
{
  const config = configured();
  config.process.detection.track_before_detect.enable = true;
  config.process.surveillance_fusion.mode = 'coherent_beam_bank';
  profiles.applyTrackingProfile(config, 'balanced');
  assert.deepEqual({profile: config.process.tracker.profile,
    pfa: config.process.detection.pfa, minDoppler: config.process.detection.minDoppler,
    M: config.process.tracker.initiate.M, N: config.process.tracker.initiate.N,
    minSnrDb: config.process.tracker.initiate.min_snr_db,
    deleteAfter: config.process.tracker.delete,
    acceleration: config.process.tracker.initiate.acceleration_model,
    tbd: config.process.detection.track_before_detect.enable},
  {profile: 'balanced', pfa: .00001, minDoppler: 15, M: 3, N: 6,
    minSnrDb: 5, deleteAfter: 10, acceleration: 'adaptive_gate', tbd: true});
  qualifyTemporalFixture(config);
  assert.equal(validateConfig(config).valid, true, validateConfig(config).errors.join('; '));
  profiles.applyTrackingProfile(config, 'permissive');
  assert.deepEqual([config.process.detection.pfa, config.process.detection.minDoppler,
    config.process.tracker.initiate.M, config.process.tracker.initiate.N,
    config.process.tracker.initiate.min_snr_db, config.process.tracker.delete],
  [.0001, 5, 2, 5, 4, 5]);
  profiles.applyTrackingProfile(config, 'conservative');
  assert.deepEqual([config.process.detection.pfa, config.process.detection.minDoppler,
    config.process.tracker.initiate.M, config.process.tracker.initiate.N,
    config.process.tracker.initiate.min_snr_db, config.process.tracker.delete],
  [.000001, 20, 4, 8, 7, 3]);
  qualifyTemporalFixture(config);
  assert.equal(validateConfig(config).valid, true, validateConfig(config).errors.join('; '));
}
{
  const config = configured();
  profiles.applyDetectionSensitivity(config, 'sensitive');
  assert.equal(config.process.detection.pfa, .0001);
  assert.equal(profiles.detectionSensitivity(config), 'sensitive');
  profiles.applyDetectionSensitivity(config, 'research');
  assert.equal(config.process.detection.pfa, .005);
  assert.equal(profiles.detectionSensitivity(config), 'research');
  profiles.applyWeakTargetMode(config, 'four');
  assert.equal(config.process.detection.track_before_detect.enable, true);
  assert.equal(config.process.detection.track_before_detect.max_cpis, 4);
  assert.equal(config.process.performance.acceleration, 'gpu-resident');
  assert.equal(config.process.surveillance_fusion.mode, source.process.surveillance_fusion.mode,
    'Power recovery preserves the selected fusion contract');
  qualifyTemporalFixture(config);
  assert.equal(validateConfig(config).valid, true, validateConfig(config).errors.join('; '));
}
{
  const config = configured();
  profiles.applyWeakTargetMode(config, 'eight');
  qualifyTemporalFixture(config);
  config.process.bearing.enable = true;
  config.process.bearing.method = 'coherent_beam_bank';
  config.process.bearing.scan_beams = 48;
  config.process.surveillance_fusion.mode = 'noise_whitened_beam_bank';
  config.process.tracker.initiate.acceleration_model = 'adaptive_gate';
  profiles.applyProcessingMode(config, 'cpu');
  assert.equal(config.process.performance.acceleration, 'cpu');
  assert.deepEqual({integration: config.process.detection.integration_cpis,
    motion: config.process.detection.integration_motion_bins,
    tbd: config.process.detection.track_before_detect.enable,
    bearing: config.process.bearing.enable,
    stationary: config.process.tracker.initiate.stationary_clutter.enable,
    acceleration: config.process.tracker.initiate.acceleration_model,
    fusion: config.process.surveillance_fusion.mode},
  {integration: 2, motion: 0, tbd: true, bearing: true,
    stationary: true, acceleration: 'adaptive_gate',
    fusion: 'noise_whitened_beam_bank'});
  assert.equal(validateConfig(config).valid, false, 'CPU cannot silently substitute the configured bearing/fusion statistic');
}

{
  const config = configured();
  profiles.applyWeakTargetMode(config, 'eight');
  qualifyTemporalFixture(config);
  config.process.tracker.beam_tolerance = 1;
  const temporal = JSON.parse(JSON.stringify(config.process.detection));
  profiles.applyProcessingMode(config, 'cpu');
  assert.equal(config.process.performance.acceleration, 'cpu');
  assert.equal(config.process.tracker.beam_tolerance, 1,
    'CPU transition preserves the explicit association gate');
  assert.deepEqual(config.process.detection, temporal,
    'Changing acceleration backend preserves the complete temporal detector');
  assert.equal(validateConfig(config).valid, false, 'unsupported beam gate must be refused');
}

{
  const config=configured();
  config.process.multi_illuminator.enabled=true;
  config.process.bearing.enable=true;
  config.process.detection.integration_cpis=2;
  config.process.detection.track_before_detect.enable=false;
  profiles.applyProcessingMode(config,'automatic');
  assert.equal(config.process.performance.acceleration,'gpu-resident',
    'Coherent lane bearing retains the resident requirement; multi-lane mode is not a blanket exception');
}

// Semantic ownership: compare complete drafts outside each action's declared
// intent. New unrelated fields automatically enter this regression contract.
function without(config, paths) {
  const result = clone(config);
  for (const path of paths) {
    const keys = path.split('.'), leaf = keys.pop();
    const parent = keys.reduce((value, key) => value?.[key], result);
    if (parent) delete parent[leaf];
  }
  return result;
}
const protectedIntent = configured();
protectedIntent.process.bearing.enable = true;
protectedIntent.process.bearing.manifold_file = '/calibration/explicit.json';
protectedIntent.process.surveillance_fusion.mode = 'noise_whitened_beam_bank';
protectedIntent.process.surveillance_fusion.manifold_file = '/calibration/detection.json';
protectedIntent.process.tracker.beam_tolerance = 1;
protectedIntent.process.detection.track_before_detect.fractional_delay = true;
for (const mode of ['cpu', 'automatic']) {
  const config = clone(protectedIntent), declared = ['process.performance.acceleration'];
  profiles.applyProcessingMode(config, mode);
  assert.deepEqual(without(config, declared), without(protectedIntent, declared),
    `${mode}: backend selection has execution intent only`);
}
for (const mode of ['off', 'two', 'two_motion', 'four', 'eight']) {
  const config = clone(protectedIntent), declared = ['process.detection.integration_cpis',
    'process.detection.integration_motion_bins', 'process.detection.track_before_detect.enable',
    'process.detection.track_before_detect.max_cpis', 'process.detection.track_before_detect.fractional_delay'];
  profiles.applyWeakTargetMode(config, mode);
  assert.deepEqual(without(config, declared), without(protectedIntent, declared),
    `${mode}: temporal intent cannot alter geometry, calibration, fusion, tracker or backend`);
}
for (const profile of getDeviceProfiles()) {
  const config = clone(protectedIntent);
  profiles.applyReceiverProfile(config, profile);
  assert.deepEqual(config.process, protectedIntent.process,
    `${profile.type}: physical receiver facts cannot rewrite radar math`);
}
for (const preset of ['pentagon5', 'square4']) {
  const config = clone(protectedIntent);
  geometry.applyPreset(config, preset);
  assert.deepEqual(without(config.process, ['reference_synthesis']),
    without(protectedIntent.process, ['reference_synthesis']),
    `${preset}: physical geometry cannot disable a configured algorithm`);
}

console.log('Composite settings preserve radar intent; incompatible execution choices are refused.');
