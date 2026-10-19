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
assert.equal(receiverCapabilities(kraken).bearing_evidence_evaluation, true);
assert.equal(receiverCapabilities(kraken).automatic_bearing_safe_selection, false);
assert.equal(receiverCapabilities(kraken).automatic_survey_execution, false);
assert.equal(receiverCapabilities(rspduo).spatial_reflection_assessment, 'two-channel-limited');
assert.equal(receiverCapabilities(rspduo).automatic_bearing_safe_selection, false);

// Each entry is a unique eligible pass: P=persistent, M=matched, U=unmatched.
function candidateFor(layout = ['PPPMUU', 'PPPMUU', 'PPPMUU']) {
  const sector_ids = layout.map((_, index) => `sector-${index}`);
  const eligible_passes = layout.flatMap((states, sector) => [...states].map((state, index) => ({
    pass_id: `pass-${sector}-${index}`, aircraft_id: `aircraft-${sector}-${index}`,
    sector_id: sector_ids[sector], matched: state !== 'U', persistent: state === 'P'
  })));
  const matched = eligible_passes.filter(pass => pass.matched);
  const observations = matched.flatMap(pass => Array.from({length: 4}, (_, index) => ({
    observation_id: `${pass.pass_id}-frame-${index}`, pass_id: pass.pass_id,
    aircraft_id: pass.aircraft_id, sector_id: pass.sector_id,
    bearing_error_deg: 9, baseline_bearing_error_deg: 10
  })));
  return {frequency_hz: 527000000, name: 'fixture', bearing_required: true,
    comparison: {site_id: 'test-site', protocol_id: 'fixed-protocol-v1',
      sector_definition_id: 'azimuth-v1', common_support_id: 'eligible-range-doppler-v1',
      baseline_id: 'ideal-manifold-v1', sector_ids},
    reference: {cnr_db: 28, clipping_fraction: 0, occupied_bandwidth_fraction: .9,
      ambiguity_pslr_db: -18, cancellation_db: 32},
    spatial: {repeat_principal_correlation: .999, ideal_correlation: .75,
      coherent_component_amplitude_span_db: 2, phase_curvature_peak_to_peak_deg: 25,
      transmitter_coordinates_verified: true},
    tracking: {matched_aircraft: matched.length, sectors: sector_ids.length,
      held_out_observations: observations.length, held_out_bearing_error_deg: 9,
      adsb_opportunities: eligible_passes.length, matched_passes: matched.length,
      persistent_passes: matched.filter(pass => pass.persistent).length,
      matched_detections: 120, persistent_tracks: matched.filter(pass => pass.persistent).length,
      eligible_passes, held_out: {fit_passes: [], observations}}};
}
const complete = candidateFor();
const evaluate = candidate => evaluateCandidate(candidate, receiverCapabilities(kraken));
const accepted = evaluate(complete);
assert.equal(accepted.status, 'qualified');
assert.equal(accepted.bearing_status, 'eligible');
assert.equal(accepted.scores.persistent_pass_rate, .5);
assert.equal(accepted.scores.matched_pass_rate, 2 / 3);
assert.equal(accepted.scores.signal, undefined);

const reflected = structuredClone(complete);
reflected.frequency_hz = 539000000;
reflected.spatial.coherent_component_amplitude_span_db = 15.9;
reflected.spatial.phase_curvature_peak_to_peak_deg = 94;
const rejected = evaluate(reflected);
assert.equal(rejected.status, 'rejected');
assert.equal(rejected.bearing_status, 'unsafe');
assert.ok(rejected.failures.includes('severe-channel-imbalance'));
assert.ok(rejected.failures.includes('severe-frequency-dependent-multipath'));

const badWaveform = structuredClone(complete);
badWaveform.reference.ambiguity_pslr_db = -3;
badWaveform.reference.cancellation_db = 8;
assert.ok(evaluate(badWaveform).failures.includes('poor-ambiguity-sidelobes'));
assert.ok(evaluate(badWaveform).failures.includes('poor-cancellation'));
const twoChannel = evaluateCandidate(complete, receiverCapabilities(rspduo));
assert.equal(twoChannel.status, 'rf-prescreen-only');
assert.equal(twoChannel.bearing_status, 'externally-unverified');
assert.ok(twoChannel.missing.includes('multi-element-spatial-reflection-assessment'));

for (const path of ['reference.ambiguity_pslr_db', 'tracking.held_out_observations',
  'tracking.eligible_passes', 'tracking.held_out.fit_passes', 'tracking.held_out.observations',
  'comparison.common_support_id', 'bearing_required']) {
  const incomplete = structuredClone(complete);
  const parts = path.split('.'), field = parts.pop();
  delete parts.reduce((value, part) => value[part], incomplete)[field];
  assert.notEqual(evaluate(incomplete).status, 'qualified', path);
  assert.equal(evaluateSurvey([incomplete], kraken).selected, null, path);
}
assert.equal(evaluateSurvey([reflected, complete], kraken).selected.frequency_hz, 527000000);
assert.equal(evaluateSurvey([complete], kraken).recommendation_only, true);
assert.throws(() => evaluateCandidate(complete, receiverCapabilities(kraken),
  {minimum_ideal_correlation: 2}), /must not exceed 1/);
assert.throws(() => evaluateCandidate(complete, receiverCapabilities(kraken),
  {minimum_adsb_opportunities: 1.5}), /integer/);
assert.throws(() => evaluateCandidate(complete, receiverCapabilities(kraken),
  {invented_gate: 1}), /Unknown illuminator survey limit/);
assert.throws(() => evaluateSurvey(Array(4097).fill(complete), kraken), /requires candidate measurements/);

// A strong reference and many detection frames cannot beat greater pass yield.
const stronger = candidateFor(['PPMMUU', 'PPMMUU', 'PPMMUU']);
stronger.frequency_hz = 539000000;
stronger.reference.cnr_db = 80;
stronger.spatial.ideal_correlation = 1;
stronger.tracking.matched_detections = 100000;
assert.equal(evaluateSurvey([stronger, complete], kraken).selected.frequency_hz, 527000000);
const moreMatches = candidateFor(['PPPMMU', 'PPPMMU', 'PPPMMU']);
moreMatches.frequency_hz = 545000000;
assert.equal(evaluateSurvey([complete, moreMatches], kraken).selected.frequency_hz, 545000000);
const betterBearing = structuredClone(complete);
betterBearing.frequency_hz = 551000000;
betterBearing.tracking.held_out.observations.forEach(item => { item.bearing_error_deg = 7; });
betterBearing.tracking.held_out_bearing_error_deg = 7;
assert.equal(evaluateSurvey([complete, betterBearing], kraken).selected.frequency_hz, 551000000);

// Unequal traffic mix is standardized by sector, not pooled across passes.
const busyEasySector = candidateFor(['PPPPPPPPPPPPPPPPPPPP', 'PMUUUU']);
busyEasySector.frequency_hz = 557000000;
const balanced = candidateFor(['PPPPMM', 'PPPPMM']);
balanced.frequency_hz = 563000000;
assert.ok(busyEasySector.tracking.persistent_passes / busyEasySector.tracking.adsb_opportunities >
  balanced.tracking.persistent_passes / balanced.tracking.adsb_opportunities);
assert.equal(evaluateSurvey([busyEasySector, balanced], kraken).selected.frequency_hz, 563000000);
const unsupportedSector = candidateFor(['PPPMUU', 'PM']);
assert.ok(evaluate(unsupportedSector).failures.includes('insufficient-sector-opportunities'));
const noPersistentPass = candidateFor(['MMMMUU', 'MMMMUU']);
noPersistentPass.tracking.persistent_tracks = 2;
assert.ok(evaluate(noPersistentPass).failures.includes('insufficient-persistent-passes'));

// Cohort differences or missing support prevent cross-trial selection.
for (const field of ['site_id', 'protocol_id', 'sector_definition_id', 'common_support_id', 'baseline_id']) {
  const other = structuredClone(complete);
  other.comparison[field] += '-different';
  const result = evaluateSurvey([complete, other], kraken);
  assert.equal(result.selected, null, field);
  assert.deepEqual(result.comparison_failures, ['incomparable-survey-cohorts']);
}
const otherTransmitter = structuredClone(complete);
otherTransmitter.comparison.frequency_hz = 600000000;
otherTransmitter.comparison.transmitter_id = 'different-transmitter';
assert.deepEqual(evaluateSurvey([complete, otherTransmitter], kraken).comparison_failures, []);
otherTransmitter.frequency_hz = 600000000;
const tie = evaluateSurvey([complete, otherTransmitter], kraken);
assert.equal(tie.selected, null);
assert.match(tie.selection_reason, /tie/);
const otherSectors = candidateFor(['PPPMUU', 'PPPMUU']);
assert.equal(evaluateSurvey([complete, otherSectors], kraken).selected, null);

// Summaries must agree with provenance; counts are passes, not detection frames.
for (const field of ['adsb_opportunities', 'matched_passes', 'persistent_passes', 'matched_detections',
  'persistent_tracks', 'matched_aircraft', 'sectors', 'held_out_observations']) {
  for (const invalid of [-1, .5, NaN, Number.MAX_SAFE_INTEGER + 1]) {
    const candidate = structuredClone(complete);
    candidate.tracking[field] = invalid;
    assert.equal(evaluate(candidate).status, 'rejected', `${field}=${invalid}`);
  }
}
for (const field of ['matched_passes', 'persistent_passes', 'matched_aircraft']) {
  const candidate = structuredClone(complete);
  candidate.tracking[field] = candidate.tracking.adsb_opportunities + 1;
  assert.equal(evaluate(candidate).status, 'rejected', field);
}
const duplicatePass = structuredClone(complete);
duplicatePass.tracking.eligible_passes.push(duplicatePass.tracking.eligible_passes[0]);
assert.ok(evaluate(duplicatePass).failures.includes('invalid-eligible-pass-provenance'));
const leakedAircraft = structuredClone(complete);
leakedAircraft.tracking.held_out.fit_passes = [{pass_id: 'different-pass',
  aircraft_id: complete.tracking.held_out.observations[0].aircraft_id}];
assert.ok(evaluate(leakedAircraft).failures.includes('held-out-data-leakage'));
const leakedPass = structuredClone(complete);
leakedPass.tracking.held_out.fit_passes = [{pass_id: complete.tracking.eligible_passes[0].pass_id,
  aircraft_id: 'different-aircraft'}];
assert.ok(evaluate(leakedPass).failures.includes('held-out-data-leakage'));
const leakedUnmatched = structuredClone(complete);
const unmatchedPass = leakedUnmatched.tracking.eligible_passes.find(pass => !pass.matched);
leakedUnmatched.tracking.held_out.fit_passes = [{pass_id: 'prior-pass', aircraft_id: unmatchedPass.aircraft_id}];
assert.ok(evaluate(leakedUnmatched).failures.includes('evaluation-data-leakage'));
const duplicateObservation = structuredClone(complete);
duplicateObservation.tracking.held_out.observations.push(duplicateObservation.tracking.held_out.observations[0]);
assert.ok(evaluate(duplicateObservation).failures.includes('invalid-held-out-provenance'));
const oneAircraft = structuredClone(complete);
oneAircraft.tracking.eligible_passes.forEach(pass => { pass.aircraft_id = 'only-aircraft'; });
oneAircraft.tracking.held_out.observations.forEach(item => { item.aircraft_id = 'only-aircraft'; });
oneAircraft.tracking.matched_aircraft = 1;
assert.ok(evaluate(oneAircraft).failures.includes('insufficient-held-out-aircraft'));
const noSector = structuredClone(complete);
noSector.tracking.held_out.observations = noSector.tracking.held_out.observations.filter(item => item.sector_id === 'sector-0');
noSector.tracking.held_out_observations = noSector.tracking.held_out.observations.length;
assert.ok(evaluate(noSector).missing.includes('tracking.held_out.sector:sector-1'));

// A global improvement may still hide a sector regression: reject it.
const sectorRegression = structuredClone(complete);
sectorRegression.tracking.held_out.observations.forEach(item => {
  item.bearing_error_deg = item.sector_id === 'sector-0' ? 11 : 5;
});
sectorRegression.tracking.held_out_bearing_error_deg = 7;
assert.ok(evaluate(sectorRegression).failures.includes('held-out-sector-regression'));
const missingBaseline = structuredClone(complete);
delete missingBaseline.tracking.held_out.observations[0].baseline_bearing_error_deg;
assert.ok(evaluate(missingBaseline).failures.includes('invalid-held-out-bearing-error'));
for (const [section, field, value] of [['reference', 'clipping_fraction', -1],
  ['reference', 'clipping_fraction', 1.01], ['reference', 'occupied_bandwidth_fraction', -.1],
  ['spatial', 'repeat_principal_correlation', 1.01], ['spatial', 'ideal_correlation', -.1],
  ['spatial', 'coherent_component_amplitude_span_db', -1]]) {
  const candidate = structuredClone(complete);
  candidate[section][field] = value;
  assert.equal(evaluate(candidate).status, 'rejected', field);
}

const radarOnly = structuredClone(complete);
radarOnly.bearing_required = false;
delete radarOnly.tracking.held_out_bearing_error_deg;
radarOnly.tracking.held_out.observations.forEach(item => {
  delete item.bearing_error_deg;
  delete item.baseline_bearing_error_deg;
});
assert.equal(evaluateCandidate(radarOnly, receiverCapabilities(rspduo)).status, 'qualified');
assert.equal(evaluateCandidate(radarOnly, receiverCapabilities(rspduo)).bearing_status, 'not-requested');
assert.equal(evaluateSurvey([complete, radarOnly], kraken).selected, null);
assert.equal(evaluateCandidate(radarOnly, receiverCapabilities({})).status, 'rejected');
const implicitBearing = structuredClone(radarOnly);
implicitBearing.bearing_required = 'false';
assert.notEqual(evaluate(implicitBearing).status, 'qualified');

console.log('Illuminator survey: pass yield, sector support, held-out safety and fail-closed validation passed.');
