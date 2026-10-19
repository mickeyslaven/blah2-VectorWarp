'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const yaml = require('js-yaml');
const {evaluateManifoldCandidate} = require('./manifold-automation');
const {setupDefaults, readConfig, saveConfig} = require('./config-store');

const polar = value => [Math.cos(value), Math.sin(value)];
const point = degrees => {
  const angle = degrees * Math.PI / 180;
  return [.25 * Math.cos(angle), .25 * Math.sin(angle), 0];
};
const config = {capture: {fc: 539000000, device: {type: 'Kraken',
  surveillance_channels: [0, 1, 2], array_geometry: {shape: 'regular_polygon',
    x_axis_bearing_deg_true: 80, elements: [0, 1, 2].map(channel =>
      ({daq_channel: channel, position: point(channel * 120)}))}}},
process: {surveillance_fusion: {mode: 'coherent_beam_bank', direct_path_null_db: 0},
  bearing: {scan_beams: 8, channel_mapping: [0, 1, 2], manifold_recorder: {
    max_elevation_deg: 15, max_delay_error_km: .25,
    max_doppler_error_hz: 5, max_truth_age_s: 3}}}};

const samples = [];
for (let bin = 0; bin < 8; ++bin) for (let pass = 0; pass < 12; ++pass) {
  const local = bin * 45, common = pass * .17;
  // Deliberately installed-site-specific phase response, not the ideal circle.
  const response = [common, common + .9 * Math.cos(local * Math.PI / 180) + .3,
    common + 1.2 * Math.sin(local * Math.PI / 180) - .4].map(polar);
  samples.push({schema: 'vectorwarp-manifold-sample-v1', frequency_hz: 539000000,
    channels: 3, config_fingerprint: 'automatic-test', icao: `${bin.toString(16)}${pass.toString(16)}`,
    radar_timestamp_ms: (bin * 100 + pass) * 300000,
    direct_path_local_azimuth_deg: 22.5,
    truth: {local_azimuth_deg: local, bearing_deg_true: (80 - local + 360) % 360,
      elevation_deg: 5, position_timestamp_s: (bin * 100 + pass) * 300},
    match: {delay_error_km: 0, doppler_error_hz: 0}, detection: {snr_db: 12},
    response, reference_weights: [polar(common), polar(common + .2), polar(common - .3)],
    bearing_replay: {reference_anchor: 'fixed_site', scan_beams: 8}});
}
const relaxed = {beams: 8, minSamples: 4, minPasses: 3, minCoverage: .75,
  maxGapDeg: 90, gates: {minimumImprovementDeg: 0, maximumErrorRatio: 1,
    maximumSectorRegressionDeg: 180}};
const result = evaluateManifoldCandidate(samples, config, relaxed);
assert.equal(result.manifold.calibration.independent_passes, 96);
assert.equal(result.evaluation.comparison.passes > 12, true);
assert.equal(result.evaluation.qualified, true,
  `balanced candidate should qualify: ${result.evaluation.failures.join(' ')}`);

const skewed = samples.filter(sample => sample.truth.local_azimuth_deg === 90);
assert.throws(() => evaluateManifoldCandidate(skewed, config, relaxed),
  /azimuth bins|unsupported azimuth gap/,
  'single-sector data cannot produce a promotable manifold');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-auto-manifold-'));
const filename = path.join(directory, 'config.yml');
const persisted = setupDefaults();
persisted.process.bearing.manifold_recorder.path = path.join(directory, 'samples.jsonl');
persisted.process.bearing.manifold_file = `${persisted.process.bearing.manifold_recorder.path}.qualified.json`;
fs.writeFileSync(filename, yaml.dump(persisted));
const document = readConfig(filename), moved = JSON.parse(JSON.stringify(document.config));
moved.location.rx.altitude += 3;
const saved = saveConfig(filename, moved, document.revision);
assert.equal(saved.config.process.bearing.manifold_file, undefined,
  'a site-defining settings change must clear an automatically selected model');

const clone = value => JSON.parse(JSON.stringify(value));
const gainConfig = yaml.load(fs.readFileSync(path.join(__dirname, '../config/config-kraken.yml'), 'utf8'));
gainConfig.capture.device.heimdall.gain = 14;
gainConfig.capture.device.heimdall.gain_control = 'fixed';
gainConfig.capture.device.array_geometry = {shape: 'regular_polygon', units: 'm',
  coordinate_frame: 'array_xy_right_handed', mapping_confirmed: true,
  geometry_confirmed: true, orientation_confirmed: true, x_axis_bearing_deg_true: 30,
  bearing_channels: [0, 1, 2, 3, 4], elements: [0, 1, 2, 3, 4].map(channel => ({
    id: `element-${channel}`, daq_channel: channel,
    position: [.2 * Math.cos(channel * 2 * Math.PI / 5), .2 * Math.sin(channel * 2 * Math.PI / 5), 0]}))};
gainConfig.process.performance.acceleration = 'gpu-resident';
gainConfig.process.tracker.gpu_capacity = 128;
gainConfig.process.surveillance_fusion.mode = 'coherent_beam_bank';
gainConfig.process.bearing.enable = true;
gainConfig.process.bearing.manifold_recorder.path = path.join(directory, 'gain-samples.jsonl');
const modelBytes = JSON.stringify(result.manifold);
for (const selection of ['automatic', 'manual', 'ideal']) {
  const source = clone(gainConfig);
  const selectedPath = selection === 'automatic' ? `${source.process.bearing.manifold_recorder.path}.qualified.json` :
    selection === 'manual' ? path.join(directory, 'manually-selected-model.json') : null;
  if (selectedPath) {
    source.process.bearing.manifold_file = selectedPath;
    fs.writeFileSync(selectedPath, modelBytes);
  }
  const gainFilename = path.join(directory, `gain-${selection}.yml`);
  fs.writeFileSync(gainFilename, yaml.dump(source));
  const initial = readConfig(gainFilename);
  assert.equal(initial.validation.valid, true, initial.validation.errors.join('; '));
  const policyOnly = clone(initial.config);
  policyOnly.capture.device.heimdall.gain_control = 'measured';
  const sameGain = saveConfig(gainFilename, policyOnly, initial.revision);
  assert.equal(sameGain.config.process.bearing.manifold_file, selectedPath || undefined,
    'Changing only gain policy must preserve selected RF qualification');
  assert.equal(sameGain.config.process.bearing.enable, true);
  const changedGain = clone(sameGain.config);
  changedGain.capture.device.heimdall.gain = 1;
  const changed = saveConfig(gainFilename, changedGain, sameGain.revision);
  assert.equal(changed.validation.valid, true, changed.validation.errors.join('; '));
  assert.equal(changed.config.process.bearing.manifold_file, undefined);
  assert.equal(changed.config.process.bearing.enable, selection === 'ideal',
    'A changed gain disables learned bearing, while existing ideal steering remains enabled');
  assert.deepEqual(changed.config.capture.device.array_geometry, source.capture.device.array_geometry,
    'RF qualification invalidation must preserve physical survey confirmations');
  for (const section of ['detection', 'tracker'])
    assert.deepEqual(changed.config.process[section], sameGain.config.process[section],
      'Radar detection and tracking must not change');
  if (selectedPath) assert.equal(fs.readFileSync(selectedPath, 'utf8'), modelBytes,
    'Invalidating selection must never rewrite or delete a learned model file');
}
fs.rmSync(directory, {recursive: true});
console.log('Manifold automation: pass-balanced holdout comparison and promotion gates passed.');
