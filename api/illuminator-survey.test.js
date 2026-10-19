'use strict';

const assert = require('assert');
const {bandPlans, receiverCapabilities, evaluateCandidate, evaluateSurvey} =
  require('./illuminator-survey');

const plans = bandPlans();
assert.equal(plans['fm-us'].candidates.length, 100);
assert.equal(plans['fm-us'].candidates[0].frequency_hz, 88100000);
assert.equal(plans['fm-us'].candidates.at(-1).frequency_hz, 107900000);
assert.equal(plans['dab-band-iii'].candidates.find(item => item.id === '12D').frequency_hz, 229072000);
assert.equal(plans['atsc-us-uhf'].candidates.length, 23);
assert.deepEqual([plans['atsc-us-uhf'].candidates[0], plans['atsc-us-uhf'].candidates.at(-1)],
  [{id: 'RF14', frequency_hz: 473000000}, {id: 'RF36', frequency_hz: 605000000}]);

const kraken = {capture: {device: {type: 'Kraken', channel_count: 5,
  array_geometry: {x_axis_bearing_deg_true: 80, elements: Array.from({length: 5}, (_, index) =>
    ({daq_channel: index, position: [index, 0, 0]}))}}}};
const rspduo = {capture: {device: {type: 'RspDuo'}}};
assert.equal(receiverCapabilities(kraken).automatic_bearing_safe_selection, true);
assert.equal(receiverCapabilities(rspduo).spatial_reflection_assessment, 'two-channel-limited');
assert.equal(receiverCapabilities(rspduo).automatic_bearing_safe_selection, false);

const complete = {frequency_hz: 527000000, name: 'fixture', bearing_required: true,
  reference: {cnr_db: 28, clipping_fraction: 0, occupied_bandwidth_fraction: .9,
    ambiguity_pslr_db: -18, cancellation_db: 32},
  spatial: {repeat_principal_correlation: .999, ideal_correlation: .75,
    coherent_component_amplitude_span_db: 2, phase_curvature_peak_to_peak_deg: 25,
    transmitter_coordinates_verified: true},
  tracking: {matched_aircraft: 6, sectors: 3, held_out_observations: 80,
    held_out_bearing_error_deg: 9, adsb_opportunities: 12, matched_detections: 120,
    persistent_tracks: 5}};
const accepted = evaluateCandidate(complete, receiverCapabilities(kraken));
assert.equal(accepted.status, 'qualified');
assert.equal(accepted.bearing_status, 'eligible');

const reflected = structuredClone(complete);
reflected.frequency_hz = 539000000;
reflected.spatial.coherent_component_amplitude_span_db = 15.9;
reflected.spatial.phase_curvature_peak_to_peak_deg = 94;
const rejected = evaluateCandidate(reflected, receiverCapabilities(kraken));
assert.equal(rejected.status, 'rejected');
assert.equal(rejected.bearing_status, 'unsafe');
assert.ok(rejected.failures.includes('severe-channel-imbalance'));
assert.ok(rejected.failures.includes('severe-frequency-dependent-multipath'));

const badWaveform = structuredClone(complete);
badWaveform.reference.ambiguity_pslr_db = -3;
badWaveform.reference.cancellation_db = 8;
const badWaveformResult = evaluateCandidate(badWaveform, receiverCapabilities(kraken));
assert.equal(badWaveformResult.status, 'rejected');
assert.ok(badWaveformResult.failures.includes('poor-ambiguity-sidelobes'));
assert.ok(badWaveformResult.failures.includes('poor-cancellation'));

const twoChannel = evaluateCandidate(complete, receiverCapabilities(rspduo));
assert.equal(twoChannel.status, 'rf-prescreen-only');
assert.equal(twoChannel.bearing_status, 'externally-unverified');
assert.ok(twoChannel.missing.includes('multi-element-spatial-reflection-assessment'));

const incomplete = structuredClone(complete);
delete incomplete.reference.ambiguity_pslr_db;
delete incomplete.tracking.held_out_observations;
assert.equal(evaluateCandidate(incomplete, receiverCapabilities(kraken)).status,
  'rf-prescreen-only');

const survey = evaluateSurvey([reflected, complete], kraken);
assert.equal(survey.selected.frequency_hz, 527000000);
assert.equal(survey.results.find(item => item.frequency_hz === 539000000).status, 'rejected');
assert.throws(() => evaluateCandidate(complete, receiverCapabilities(kraken),
  {minimum_ideal_correlation: 2}), /must not exceed 1/);
assert.throws(() => evaluateCandidate(complete, receiverCapabilities(kraken),
  {invented_gate: 1}), /Unknown illuminator survey limit/);
assert.throws(() => evaluateSurvey(Array(4097).fill(complete), kraken),
  /requires candidate measurements/);

const radarOnly = structuredClone(complete);
radarOnly.bearing_required = false;
delete radarOnly.tracking.held_out_bearing_error_deg;
assert.equal(evaluateCandidate(radarOnly, receiverCapabilities(rspduo)).status, 'qualified');

console.log('Illuminator band plans, receiver limits, radar evidence and reflection veto passed.');
