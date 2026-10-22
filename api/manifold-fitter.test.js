'use strict';

const assert = require('assert');
const {fitManifold} = require('./manifold-fitter');

const samples = [];
for (let bin = 0; bin < 12; ++bin) for (let sample = 0; sample < 10; ++sample) {
  const angle = bin * 30 * Math.PI / 180, common = sample * .7;
  const phases = [common, common + 1.1 * Math.cos(angle), common + .8 * Math.sin(angle)];
  samples.push({schema: 'vectorwarp-manifold-sample-v1', frequency_hz: 539000000,
    channels: 3, config_fingerprint: 'one-site',
    icao: `a${bin}${sample}`, radar_timestamp_ms: (bin * 10 + sample) * 300000,
    direct_path_local_azimuth_deg: 0,
    truth: {local_azimuth_deg: bin * 30, elevation_deg: 5 + sample},
    detection: {snr_db: 12}, response: phases.map(phase => [Math.cos(phase), Math.sin(phase)]),
    reference_weights: [common, common + 1.1, common].map(phase => [Math.cos(phase), Math.sin(phase)])});
}
const result = fitManifold(samples, {beams: 12, minSamples: 3, minCoverage: 1, maxGapDeg: 45});
assert.equal(result.schema, 'vectorwarp-bearing-manifold-v1');
assert.equal(result.responses.length, 12);
assert.ok(result.responses.every(row => row.length === 3));
assert.equal(result.calibration.observed_bins, 12);
assert.equal(result.calibration.interpolated_bins, 0);
assert.equal(result.calibration.independent_passes, 120);
assert.equal(result.calibration.unique_aircraft, 120);
assert.equal(result.calibration.sector_coverage.length, 8);
assert.ok(result.calibration.holdout_median_correlation > .99);
const contaminated = {...samples[0], truth: {...samples[0].truth, elevation_deg: 45}};
const filtered = fitManifold([...samples, contaminated],
  {beams: 12, minSamples: 3, minCoverage: 1, maxGapDeg: 45});
assert.equal(filtered.calibration.supplied_samples, 121);
assert.equal(filtered.calibration.input_samples, 120);
assert.equal(filtered.calibration.rejected_samples.elevation, 1);
const noInternalHoldout = fitManifold(samples, {beams: 12, minSamples: 3,
  minCoverage: 1, maxGapDeg: 45, holdoutPredicate: () => false});
assert.equal(noInternalHoldout.calibration.holdout_bins, 0);
assert.ok(noInternalHoldout.calibration.bins.every(row => row.holdout_samples === 0));
assert.throws(() => fitManifold([...samples, {...samples[0], config_fingerprint: 'other'}],
  {beams: 12, minSamples: 3}), /different frequencies, channel layouts, or site configurations/);
console.log('Manifold fitter: common-phase removal, bin fitting, holdout validation, and config isolation passed.');
