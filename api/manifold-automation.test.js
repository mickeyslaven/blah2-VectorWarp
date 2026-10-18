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
fs.rmSync(directory, {recursive: true});
console.log('Manifold automation: pass-balanced holdout comparison and promotion gates passed.');
