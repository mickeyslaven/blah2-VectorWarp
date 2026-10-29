'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const profiles = require('../html/js/config_profiles');
const geometry = require('../html/js/kraken_geometry');
const {validateConfig} = require('./config-manager');

const source = yaml.load(fs.readFileSync(path.join(__dirname, '..', 'config', 'config-kraken.yml'), 'utf8'));
const clone = value => JSON.parse(JSON.stringify(value));
const configured = () => {
  const config = clone(source);
  config.capture.device.channel_count = 5;
  geometry.applyPreset(config, 'pentagon5');
  config.capture.device.array_geometry.x_axis_bearing_deg_true = 0;
  config.process.performance = {...config.process.performance, acceleration: 'gpu-resident'};
  config.process.tracker.gpu_capacity = 64;
  config.process.tracker.initiate.stationary_clutter = {enable: true, persistence_cpis: 12, max_doppler_hz: 0};
  return config;
};

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
  profiles.applyClutterMode(config, 'full_window');
  assert.equal(profiles.clutterMode(config), 'full_window');
  assert.deepEqual([config.process.clutter.delayMin, config.process.clutter.delayMax],
    [config.process.ambiguity.delayMin, config.process.ambiguity.delayMax]);
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
  assert.equal(config.process.surveillance_fusion.mode, 'coherent_beam_bank');
  assert.equal(validateConfig(config).valid, true, validateConfig(config).errors.join('; '));
}
{
  const config = configured();
  profiles.applyWeakTargetMode(config, 'eight');
  config.process.bearing.enable = true;
  config.process.bearing.method = 'coherent_beam_bank';
  config.process.bearing.scan_beams = 48;
  config.process.tracker.track_guided_shadow = {enable: true, promote: true, max_tracks: 32};
  config.process.surveillance_fusion.mode = 'noise_whitened_beam_bank';
  config.process.tracker.initiate.acceleration_model = 'adaptive_gate';
  profiles.applyProcessingMode(config, 'cpu');
  assert.equal(config.process.performance.acceleration, 'cpu');
  assert.deepEqual({integration: config.process.detection.integration_cpis,
    motion: config.process.detection.integration_motion_bins,
    tbd: config.process.detection.track_before_detect.enable,
    bearing: config.process.bearing.enable,
    guided: config.process.tracker.track_guided_shadow.enable,
    promote: config.process.tracker.track_guided_shadow.promote,
    stationary: config.process.tracker.initiate.stationary_clutter.enable,
    acceleration: config.process.tracker.initiate.acceleration_model,
    fusion: config.process.surveillance_fusion.mode},
  {integration: 1, motion: 0, tbd: false, bearing: false, guided: false,
    promote: false, stationary: false, acceleration: 'hypothesis_bank',
    fusion: 'coherent_beam_bank'});
  assert.equal(validateConfig(config).valid, true, validateConfig(config).errors.join('; '));
}

console.log('Composite settings map to validated backend configuration and CPU-only disables GPU-resident work.');
