'use strict';

const assert = require('assert');
const {evaluateBearing, evaluateReferenceDirection, evaluateReferenceSurvey, mappings,
  steeringWeights, inverse} = require('./bearing-diagnostic');

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
      reference_anchor: 'fixed_site',
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
assert.equal(result.reference_anchor, 'fixed_site');
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
const transmitterBearing = 0, knownMapping = mappings(channels).find(item => item.id === 'forward-3');
function expectedReference(mapping, heading, bearing = transmitterBearing, frequency = 539000000) {
  const direct = (heading - bearing) * Math.PI / 180;
  const wave = 2 * Math.PI * frequency / 299792458;
  const positions = config.capture.device.array_geometry.elements.map(item => item.position);
  const raw = positions.map((_, channel) => {
    const position = positions[mapping[channel]];
    const phase = wave * (position[0] * Math.cos(direct) + position[1] * Math.sin(direct));
    return [Math.cos(phase), Math.sin(phase)];
  });
  const anchorPhase = Math.atan2(raw[0][1], raw[0][0]);
  return raw.map(value => ({amplitude_db: 0,
    phase_deg: ((Math.atan2(value[1], value[0]) - anchorPhase) * 180 / Math.PI + 540) % 360 - 180}));
}
const referenceConfig = JSON.parse(JSON.stringify(config));
referenceConfig.location = {rx: {latitude: 40, longitude: -80},
  tx: {latitude: 41, longitude: -80, name: 'Known transmitter'}};
const referenceChannels = expectedReference(knownMapping.mapping, 80);
const referenceRows = Array.from({length: 40}, (_, index) => ({
  schema: 'vectorwarp-reference-drift-v1', frequency_hz: 539000000,
  radar_timestamp_ms: 1000000 + index * 10000, config_fingerprint: 'site-a',
  relative_eigenvector: {anchor_channel: 0, channels: referenceChannels},
  reference_state: {coherent_fraction: .84}}));
const referenceResult = evaluateReferenceDirection(referenceRows, referenceConfig);
assert.equal(referenceResult.available, true);
assert.equal(referenceResult.quality.updates, 40);
assert.equal(referenceResult.quality.reliable, true);
assert.deepEqual(referenceResult.best_mapping_at_configured_heading.mapping, knownMapping.mapping);
assert.ok(referenceResult.best_mapping_at_configured_heading.median_phase_error_deg < 1e-6);
assert.deepEqual(referenceResult.best_mapping_at_configured_heading.channel_median_phase_error_deg,
  [null, 0, 0, 0, 0]);
assert.ok(referenceResult.configured.median_phase_error_deg > 20);
assert.ok(referenceResult.heading_with_configured_mapping.median_phase_error_deg < 1);
assert.equal(referenceResult.auto_apply, false);
assert.match(referenceResult.interpretation, /independent known-angle source/i);

const surveyConfig = JSON.parse(JSON.stringify(referenceConfig));
surveyConfig.location.rx = {latitude: 0, longitude: 0};
surveyConfig.process.bearing.channel_mapping = knownMapping.mapping;
function surveyRows(bearing, frequency, fingerprint) {
  const values = expectedReference(knownMapping.mapping, 80, bearing, frequency);
  return Array.from({length: 12}, (_, index) => ({
    schema: 'vectorwarp-reference-drift-v1', frequency_hz: frequency,
    radar_timestamp_ms: 2000000 + index * 10000, config_fingerprint: fingerprint,
    relative_eigenvector: {anchor_channel: 0, channels: values},
    reference_state: {coherent_fraction: .95}}));
}
const surveyResult = evaluateReferenceSurvey([
  {name: 'North reference', latitude: 1, longitude: 0, frequency_hz: 527000000,
    reference_updates: surveyRows(0, 527000000, 'north')},
  {name: 'East reference', latitude: 0, longitude: 1, frequency_hz: 485000000,
    reference_updates: surveyRows(90, 485000000, 'east')}
], surveyConfig);
assert.equal(surveyResult.available, true);
assert.deepEqual(surveyResult.reliable_sources, ['North reference', 'East reference']);
assert.deepEqual(surveyResult.configured_mapping.mapping, knownMapping.mapping);
assert.equal(surveyResult.physical_ch0_heading_deg_true, 80);
assert.equal(surveyResult.configured_mapping.fitted_phase_axis_heading_deg_true, 80);
assert.ok(surveyResult.configured_mapping.median_phase_error_deg < 1e-6);
assert.equal(surveyResult.auto_apply, false);
const rejectedSurvey = evaluateReferenceSurvey([{name: 'Poor reference', latitude: 1,
  longitude: 0, frequency_hz: 527000000, reference_updates: surveyRows(0, 527000000, 'poor')
    .map(row => ({...row, reference_state: {coherent_fraction: .5}}))}], surveyConfig);
assert.equal(rejectedSurvey.available, false);
assert.match(rejectedSurvey.reason, /quality gate/i);

const projectedConfig = JSON.parse(JSON.stringify(config));
projectedConfig.process.bearing.reference_anchor = 'fixed_site_projected';
const projectedSamples = JSON.parse(JSON.stringify(samples));
projectedSamples[0].bearing_replay.reference_anchor = 'fixed_site_projected';
for (const [candidate, label] of [[projectedConfig, 'configuration'],
  [config, 'evidence']]) {
  const evaluated = evaluateBearing(label === 'evidence' ? projectedSamples : samples, candidate);
  assert.equal(evaluated.available, false, `${label} must remain outside ordinary diagnostics`);
  assert.match(evaluated.reason, /experimental projected replay evidence/i);
}
console.log('Bearing diagnostic: complex channel permutations, exact covariance scoring, and safe shadow-only reporting passed.');
