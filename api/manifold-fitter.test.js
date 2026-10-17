'use strict';

const assert = require('assert');
const {fitManifold} = require('./manifold-fitter');

const samples = [];
for (let bin = 0; bin < 12; ++bin) for (let sample = 0; sample < 5; ++sample) {
  const angle = bin * 30 * Math.PI / 180, common = sample * .7;
  const phases = [common, common + 1.1 * Math.cos(angle), common + .8 * Math.sin(angle)];
  samples.push({schema: 'vectorwarp-manifold-sample-v1', frequency_hz: 539000000,
    channels: 3, config_fingerprint: 'one-site',
    direct_path_local_azimuth_deg: 0,
    truth: {local_azimuth_deg: bin * 30, elevation_deg: 12 + sample},
    detection: {snr_db: 12}, response: phases.map(phase => [Math.cos(phase), Math.sin(phase)]),
    reference_weights: [common, common + 1.1, common].map(phase => [Math.cos(phase), Math.sin(phase)])});
}
const result = fitManifold(samples, {beams: 12, minSamples: 3, minCoverage: 1, maxGapDeg: 45});
assert.equal(result.schema, 'vectorwarp-bearing-manifold-v1');
assert.equal(result.responses.length, 12);
assert.ok(result.responses.every(row => row.length === 3));
assert.equal(result.calibration.observed_bins, 12);
assert.equal(result.calibration.interpolated_bins, 0);
assert.ok(result.calibration.holdout_median_correlation > .99);
assert.throws(() => fitManifold([...samples, {...samples[0], config_fingerprint: 'other'}],
  {beams: 12, minSamples: 3}), /different frequencies, channel layouts, or site configurations/);
console.log('Manifold fitter: common-phase removal, bin fitting, holdout validation, and config isolation passed.');
