'use strict';

const assert = require('assert');
const {analysisDescriptor, analysisFingerprint, summarizeCovariances, idealVector,
  transmitterBearing} = require('./frequency-resolved-analysis');

const channels = 5, subbands = 24, radius = .193548;
const config = {capture: {fc: 527000000, fs: 2400000, device: {type: 'Kraken',
  channel_count: channels, surveillance_channels: [0, 1, 2, 3, 4],
  array_geometry: {x_axis_bearing_deg_true: 80,
    elements: Array.from({length: channels}, (_, channel) => ({daq_channel: channel,
      position: [radius * Math.cos(-2 * Math.PI * channel / channels),
        radius * Math.sin(-2 * Math.PI * channel / channels), 0]}))}}},
process: {bearing: {frequency_resolved_shadow: {fft_size: 2048, subbands,
  bandwidth_fraction: .9, diagonal_loading: .01}}},
location: {rx: {latitude: 40.442, longitude: -80.0567},
  tx: {latitude: 40.46325, longitude: -80.0043}}};
const descriptor = analysisDescriptor(config);
assert.equal(descriptor.channels, channels);
assert.equal(analysisFingerprint(descriptor).length, 64);
assert.ok(transmitterBearing(config.location.rx, config.location.tx) > 0);
const quarter = idealVector({positions_m: [[0, 0, 0],
  [299792458 / descriptor.frequency_hz / 4, 0, 0]],
  direct_path_local_azimuth_deg: 0}, descriptor.frequency_hz);
assert.ok(Math.abs(quarter[1][0]) < 1e-12 && quarter[1][1] < 0,
  'native direct response uses exp(-j k.r), independently of the production helper');

const covariance = vector => vector.map((left, row) => vector.map((right, column) => {
  const value = [left[0] * right[0] + left[1] * right[1],
    left[1] * right[0] - left[0] * right[1]];
  if (row === column) value[0] += .01;
  return value;
}));
const usable = descriptor.sample_rate_hz * descriptor.bandwidth_fraction;
const directOnly = Array.from({length: subbands}, (_, band) => {
  const offset = -usable / 2 + usable * (band + .5) / subbands;
  return covariance(idealVector(descriptor, descriptor.frequency_hz + offset));
});
const clean = summarizeCovariances(directOnly, descriptor, {fft_blocks: 1});
assert.ok(clean.summary.ideal_correlation.min > .999);
assert.ok(clean.summary.rank1_fraction.min > .95);
assert.equal(clean.subbands.length, subbands);
assert.equal(clean.subbands[0].direct_constrained_mvdr.weights.length, channels);

const reflected = Array.from({length: subbands}, (_, band) => {
  const offset = -usable / 2 + usable * (band + .5) / subbands;
  const direct = idealVector(descriptor, descriptor.frequency_hz + offset);
  const alternate = {...descriptor, direct_path_local_azimuth_deg: 250};
  const reflection = idealVector(alternate, descriptor.frequency_hz + offset);
  const phase = -2 * Math.PI * offset * 30e-9;
  const scalar = [2 * Math.cos(phase), 2 * Math.sin(phase)];
  return covariance(direct.map((value, channel) => [value[0] +
    scalar[0] * reflection[channel][0] - scalar[1] * reflection[channel][1],
  value[1] + scalar[0] * reflection[channel][1] + scalar[1] * reflection[channel][0]]));
});
const mixed = summarizeCovariances(reflected, descriptor, {fft_blocks: 1});
assert.ok(mixed.summary.ideal_correlation.median < .9);
assert.ok(Math.max(...mixed.summary.channel_frequency_phase.map(row =>
  Math.abs(row.phase_excursion_deg))) > 5);

const invalid = structuredClone(config);
invalid.process.bearing.frequency_resolved_shadow.fft_size = 1000;
assert.throws(() => analysisDescriptor(invalid), /Invalid frequency-resolved/);
console.log('Frequency-resolved reference: generic geometry, direct constraint, multipath phase slope, and validation passed.');
