'use strict';

const assert = require('assert');
const {evaluateBearing, mappings, steeringWeights, inverse} = require('./bearing-diagnostic');

const channels = 5, beams = 60, radius = .19;
const config = {capture: {device: {surveillance_channels: [0, 1, 2, 3, 4],
  array_geometry: {shape: 'regular_polygon', x_axis_bearing_deg_true: 80,
    elements: Array.from({length: channels}, (_, daq_channel) => ({daq_channel,
      position: [radius * Math.cos(-2 * Math.PI * daq_channel / channels),
        radius * Math.sin(-2 * Math.PI * daq_channel / channels), 0]}))}}},
process: {surveillance_fusion: {mode: 'coherent_beam_bank', direct_path_null_db: 0},
  bearing: {scan_beams: beams}}};
const desired = mappings(channels).find(item => item.id === 'reverse-2');
const reference = Array.from({length: channels}, (_, channel) =>
  [Math.cos(channel * .37) / Math.sqrt(channels), Math.sin(channel * .37) / Math.sqrt(channels)]);
const samples = [];
for (let aircraft = 0; aircraft < 5; ++aircraft) for (let index = 0; index < 20; ++index) {
  const beam = (aircraft * 11 + index * 2) % beams;
  const seed = {frequency_hz: 539000000, channels, reference_weights: reference,
    direct_path_local_azimuth_deg: 51, bearing_replay: {method: 'covariance_mvdr',
      scan_beams: beams, guard_radius: 1, training_radius: 3, diagonal_loading: .01,
      channel_mapping: [0, 1, 2, 3, 4]}};
  const bank = steeringWeights(seed, config, desired.mapping, beams);
  samples.push({...seed, schema: 'vectorwarp-manifold-sample-v1', icao: `abc12${aircraft}`,
    truth: {bearing_deg_true: ((80 - beam * 360 / beams) % 360 + 360) % 360},
    response: bank[beam].map(value => [value[0], -value[1]]),
    covariance_samples: 40,
    covariance: Array.from({length: channels}, (_, row) =>
      Array.from({length: channels}, (_, column) => [row === column ? 1 : 0, 0]))});
}
const result = evaluateBearing(samples, config);
assert.equal(result.available, true);
assert.equal(result.evidence_ready, true);
assert.equal(result.samples, 100);
assert.deepEqual(result.best.mapping, desired.mapping);
assert.ok(result.best.median_error_deg < 1e-6);
assert.equal(result.auto_apply, false);
assert.match(result.interpretation, /no result changes live geometry/i);
assert.match(result.identifiability, /mathematically degenerate/i);
assert.deepEqual(inverse([[[2, 0], [0, 0]], [[0, 0], [4, 0]]]),
  [[[.5, 0], [0, 0]], [[0, 0], [.25, 0]]]);

const unsupported = JSON.parse(JSON.stringify(config));
unsupported.process.surveillance_fusion.mode = 'noise_whitened_beam_bank';
assert.equal(evaluateBearing(samples, unsupported).available, false);
console.log('Bearing diagnostic: complex channel permutations, exact covariance scoring, and safe shadow-only reporting passed.');
