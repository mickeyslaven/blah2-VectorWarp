'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const os = require('os');
const {execFileSync} = require('child_process');
const yaml = require('js-yaml');
const {setupDefaults, configurationForReceiver} = require('./config-store');
const {getDeviceProfiles, validateConfig} = require('./config-manager');

const configFiles = fs.readdirSync(path.join(__dirname, '..', 'config'))
  .filter(name => /^config(?:-[a-z0-9-]+)?\.yml$/.test(name)).sort();
assert.deepStrictEqual(configFiles, ['config.yml'],
  'Receiver acquisition profiles must not duplicate the canonical radar configuration');
const profiles = ['Unconfigured', ...getDeviceProfiles().map(profile => profile.type)];
const load = type => type === 'Unconfigured' ? setupDefaults() : configurationForReceiver(type);
for (const depth of [2, 4, 8]) {
  const requested = load('Kraken');
  requested.process.performance.acceleration = 'cpu';
  requested.process.detection.integration_cpis = 2;
  requested.process.detection.integration_motion_bins = 1;
  Object.assign(requested.process.detection.track_before_detect,
    {enable: depth > 2, max_cpis: Math.max(4, depth), fractional_delay: depth > 2});
  requested.process.tracker.initiate.N = 1 + depth * (requested.process.tracker.initiate.M - 1);
  requested.process.tracker.delete = Math.max(requested.process.tracker.delete, depth - 1);
  for (const {type} of getDeviceProfiles()) {
    const selected = configurationForReceiver(type, requested);
    if (type === 'HackRF') selected.capture.device.serial = ['0001', '0002'];
    assert.deepStrictEqual(selected.process.detection, requested.process.detection,
      `${type}: receiver selection silently downgraded ${depth}-CPI temporal processing`);
    assert.deepStrictEqual(selected.process.reference_synthesis, requested.process.reference_synthesis,
      `${type}: receiver selection silently rewrote the requested array reference`);
    const validation = validateConfig(selected);
    if (type === 'Kraken') assert.equal(validation.valid, true, validation.errors.join('; '));
    else {
      assert.equal(validation.valid, false, 'five-channel reference intent requires explicit role repair');
      assert.ok(validation.errors.includes('capture channel roles must be unique, in range, and identify one disjoint dedicated reference or a valid array reference'));
      assert.ok(validation.errors.includes('process.reference_synthesis.channels: input 4 is outside the configured channel count'));
    }
  }
}
for (const {type} of getDeviceProfiles()) {
  const fresh = load(type);
  if (type === 'HackRF') fresh.capture.device.serial = ['0001', '0002'];
  const validation = validateConfig(fresh);
  assert.equal(validation.valid, true, `${type}: fresh configuration ${validation.errors.join('; ')}`);
}
const acquisitionDefaults = {
  Kraken: {rate: 2400000, host: '127.0.0.1', port: 8091, controlPort: 8092},
  Usrp: {rate: 2000000, address: 'type=b200', subdev: 'A:A A:B', antenna: ['RX2', 'RX2'], gain: [20, 20]},
  RspDuo: {rate: 2000000, agcSetPoint: -20, bandwidthNumber: 5, gainReduction: [50, 45], lnaState: 1},
  HackRF: {rate: 2000000, gain_lna: [32, 32], gain_vga: [30, 30], amp_enable: [false, false]}
};
for (const [type, expected] of Object.entries(acquisitionDefaults)) {
  const capture = load(type).capture;
  assert.equal(capture.fs, expected.rate, `${type}: acquisition sample rate`);
  const device = type === 'Kraken' ? {...capture.device.heimdall,
    controlPort: capture.device.heimdall.control_port} : capture.device;
  for (const [key, value] of Object.entries(expected))
    if (key !== 'rate') assert.deepStrictEqual(device[key], value, `${type}: ${key}`);
}
assert.deepStrictEqual(setupDefaults(), yaml.load(fs.readFileSync(
  path.join(__dirname, '..', 'config/config.yml'), 'utf8')),
  'The API and installed first-run template must share one canonical default set');
// Installed Linux releases keep the immutable template in config-examples/;
// the source/macOS config/ layout above must not be an accidental requirement.
const packaged = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-defaults-package-'));
try {
  fs.mkdirSync(path.join(packaged, 'api'));
  fs.mkdirSync(path.join(packaged, 'config-examples'));
  fs.copyFileSync(path.join(__dirname, '../config/config.yml'), path.join(packaged, 'config-examples/config.yml'));
  for (const file of ['config-store.js', 'config-generate.js'])
    fs.copyFileSync(path.join(__dirname, file), path.join(packaged, 'api', file));
  for (const file of ['config-manager.js', 'config-rules.js', 'manifold-recorder.js'])
    fs.symlinkSync(path.join(__dirname, file), path.join(packaged, 'api', file));
  fs.symlinkSync(path.join(__dirname, '../html'), path.join(packaged, 'html'));
  fs.symlinkSync(path.dirname(path.dirname(require.resolve('js-yaml/package.json'))),
    path.join(packaged, 'api/node_modules'));
  const installed = require(path.join(packaged, 'api/config-store'));
  assert.deepStrictEqual(installed.setupDefaults(), setupDefaults());
  for (const profile of getDeviceProfiles()) {
    const generated = yaml.load(execFileSync(process.execPath,
      [path.join(packaged, 'api/config-generate.js'), profile.type], {encoding: 'utf8', timeout: 10000}));
    assert.deepStrictEqual(generated, configurationForReceiver(profile.type),
      `${profile.type}: installed generator changed the effective configuration`);
  }
} finally {
  fs.rmSync(packaged, {recursive: true, force: true});
}
// Reference formation precedes the canonical downstream radar graph. The
// five coherent Kraken channels explicitly synthesize their reference; paired
// adapters use a dedicated reference. This is a topology declaration, not an
// excuse for receiver-dependent clutter, detector, temporal or tracker code.
const referenceTopology = {
  HackRF: 'dedicated',
  Kraken: 'array_eigenbeam',
  RspDuo: 'dedicated',
  Usrp: 'dedicated',
  Unconfigured: 'dedicated'
};
for (const profile of profiles) {
  const value = load(profile);
  assert.equal(value.save.iq, undefined);
  assert.equal(value.save.timing, undefined);
  if (profile === 'HackRF') value.capture.device.serial = ['0001', '0002'];
  assert.equal(validateConfig(value).valid, profile !== 'Unconfigured',
    `${profile}: ${validateConfig(value).errors.join('; ')}`);
  assert.equal(value.process.reference_synthesis?.mode ?? 'dedicated',
    referenceTopology[profile], `${profile}: undeclared reference topology`);
  if (referenceTopology[profile] === 'array_eigenbeam') {
    assert.equal(value.capture.device.channel_count, 5);
    assert.deepStrictEqual(value.process.reference_synthesis.channels, [0, 1, 2, 3, 4]);
    assert.deepStrictEqual(value.capture.device.surveillance_channels, [0, 1, 2, 3, 4]);
  }
}
const select = value => {
  const process = value.process;
  const detection = structuredClone(process.detection);
  const tracker = process.tracker;
  const reference = process.reference_synthesis || {};
  const fusion = process.surveillance_fusion || {};
  const rate = process.delay_reference_sample_rate_hz ?? value.capture.fs;
  const km = bins => bins * 299792.458 / rate;
  assert.ok(Number.isFinite(rate) && rate > 0);
  // Compare requested physical intent before adapter integer-bin rounding.
  // Reference rate is a bin-coordinate convention, NOT the acquisition rate.
  // Native processing projects these physical requests onto each actual lane.
  // Rate/channel count/buffer/workers and declared reference topology may
  // differ; downstream physical detector/tracker defaults may not.
  for (const key of ['nGuard', 'nTrain', 'minDelay', 'nCentroid']) {
    detection[`${key}_km`] = km(detection[key]);
    delete detection[key];
  }
  return {
    cpi_seconds: process.data.cpi,
    acceleration: process.performance?.acceleration ?? 'auto',
    channelization: process.multi_illuminator,
    ambiguity: {delayMin_km: km(process.ambiguity.delayMin),
      delayMax_km: km(process.ambiguity.delayMax),
      dopplerMin_hz: process.ambiguity.dopplerMin, dopplerMax_hz: process.ambiguity.dopplerMax},
    clutter: {enable: process.clutter.enable, followAmbiguity: process.clutter.followAmbiguity,
      delayMin_km: km(process.clutter.delayMin), delayMax_km: km(process.clutter.delayMax)},
    reference: {analysis_samples: reference.analysis_samples ?? 32768,
      analysis_interval: reference.analysis_interval ?? 10,
      power_iterations: reference.power_iterations ?? 12,
      covariance_smoothing: reference.covariance_smoothing ?? .8,
      diagonal_loading: reference.diagonal_loading ?? .001,
      target_protection_enabled: reference.target_protection_enabled ?? false},
    fusion: {mode: fusion.mode ?? 'noncoherent', beams: fusion.beams ?? 12,
      effective_beam_trials: fusion.effective_beam_trials ?? 12,
      direct_path_null_db: fusion.direct_path_null_db ?? 0},
    detector: detection,
    bearing: {enable: process.bearing?.enable ?? false,
      method: process.bearing?.method ?? 'coherent_beam_bank',
      reference_anchor: process.bearing?.reference_anchor ?? 'live_eigenbeam',
      scan_beams: process.bearing?.scan_beams ?? 48,
      min_consistent: process.bearing?.min_consistent ?? 3,
      min_peak_separation_db: process.bearing?.min_peak_separation_db ?? 3},
    tracker: {enable: tracker.enable, profile: tracker.profile,
      initiate: tracker.initiate, delete: tracker.delete}
  };
};

const baseline = select(load('Usrp'));
const mismatches = [];
function differences(actual, expected, location, result = []) {
  if (expected && typeof expected === 'object' && !Array.isArray(expected)) {
    for (const key of new Set([...Object.keys(actual || {}), ...Object.keys(expected)]))
      differences(actual?.[key], expected[key], `${location}.${key}`, result);
  } else if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    result.push(`${location}: actual=${JSON.stringify(actual)}; canonical=${JSON.stringify(expected)}`);
  }
  return result;
}
for (const profile of profiles) mismatches.push(...differences(select(load(profile)), baseline, profile));
assert.deepStrictEqual(mismatches, [], 'Generated receiver configurations changed the shared radar defaults:\n' + mismatches.join('\n'));

// Negative controls prove this semantic contract sees the historical drift,
// even when the configured integer bins are identical across receivers.
const oldKraken = load('Kraken');
oldKraken.process.delay_reference_sample_rate_hz = 2400000;
assert.equal(differences(select(oldKraken), baseline, 'old-kraken').length, 8);
const oldRsp = load('RspDuo');
oldRsp.process.ambiguity.dopplerMin = -300; oldRsp.process.ambiguity.dopplerMax = 300;
assert.equal(differences(select(oldRsp), baseline, 'old-rsp').length, 2);
const oldDefault = load('Unconfigured');
oldDefault.process.data.cpi = .75; oldDefault.process.tracker.enable = false;
assert.equal(differences(select(oldDefault), baseline, 'old-default').length, 2);

console.log('Unified radar contract: one canonical configuration generates all receiver profiles with shared physical radar defaults.');
