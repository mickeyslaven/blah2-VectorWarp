'use strict';

const assert = require('assert/strict');
const {inverse} = require('./bistatic-geolocation');
const {localPoint3d, geographicPoint3d, excessPath3d, fuseTrackCohort,
  validateFusionAgainstAdsb, MultiIlluminatorTrackFuser} = require('./multi-illuminator-track-fusion');

const C = 299792.458;
const receiver = {latitude: 40, longitude: -80, altitude: 250};
const illuminators = [
  {id: 'east', enabled: true, frequency_hz: 500e6, output_sample_rate_hz: 2.4e6,
    transmitter: {latitude: 40, longitude: -79.75, altitude: 400}},
  {id: 'north', enabled: true, frequency_hz: 510e6, output_sample_rate_hz: 2.4e6,
    transmitter: {latitude: 40.22, longitude: -80, altitude: 500}},
  {id: 'west', enabled: true, frequency_hz: 520e6, output_sample_rate_hz: 2.4e6,
    transmitter: {latitude: 40.02, longitude: -80.22, altitude: 300}}
];
const target = {latitude: 40.11, longitude: -79.91, altitude: 6000};
const configFor = (definitions = illuminators, rx = receiver, localization = {}) => ({
  location: {rx}, process: {multi_illuminator: {enabled: true, illuminators: definitions, localization},
    tracker: {initiate: {M: 3, N: 6}, delete: 3}}});
const config = configFor();
function cohort(definitions = illuminators, point = target, index = 0, rx = receiver, epoch = 4) {
  const local = localPoint3d(rx, point);
  return definitions.map(definition => ({id: definition.id, sample_rate_hz: definition.output_sample_rate_hz,
    capture_sample_position_available: true, capture_epoch: epoch,
    capture_first_input_sample: 1000 + index * 1000, capture_end_input_sample: 2000 + index * 1000,
    timestamp: 100000 + index * 1000,
    tracks: {timestamp: 100000 + index * 1000, data: [{id: `${definition.id}-track`, state: 'ACTIVE',
      delay: excessPath3d(local, localPoint3d(rx, definition.transmitter)) /
        (C / definition.output_sample_rate_hz), doppler: 30}]}}));
}
const resolved = result => result.hypotheses.filter(item => item.resolved);
const assertDiagnostic = (result, label) => {
  assert.equal(result.operational_localization, false, label);
  assert.equal(resolved(result).length, 0, label);
};

// Nonzero receiver, transmitter and aircraft heights enter the same 3-D model.
for (const altitude of [3000, 6000, 15000]) {
  const point = {...target, altitude};
  const fused = fuseTrackCohort(cohort(illuminators, point), config);
  assert.equal(fused.available, true);
  assert.equal(resolved(fused).length, 1);
  const solution = resolved(fused)[0];
  assert.ok(inverse(point, solution).distanceM < .01);
  assert.ok(Math.abs(solution.altitude - altitude) < .01);
  assert.equal(solution.support, 3);
  assert.equal(solution.lane_support, 3);
  assert.equal(solution.altitude_model, 'three-dimensional-wgs84-ellipsoid');
  assert.equal(solution.uncertainty.calibrated, false);
  assert.equal(solution.uncertainty.kind, 'estimated');
  assert.equal(solution.uncertainty.independent_range_count, 3);
  assert.equal(solution.uncertainty.residual_degrees_of_freedom, 0,
    'Exactly determined geometry is not presented as residual validation');
  assert.ok(solution.uncertainty.vertical_sigma_km > 0);
}
const roundTrip = geographicPoint3d(receiver, localPoint3d(receiver, target));
assert.ok(Math.abs(roundTrip.altitude - target.altitude) < 1e-5);

// ADS-B is a read-only validation input. Changed, removed and adversarial truth
// leave the exact serialized solver result unchanged across complete runs.
const runs = [[], [{lat: target.latitude, lon: target.longitude, alt_geom: target.altitude / .3048}],
  [{lat: -30, lon: 120, alt_geom: 1000000}]].map(aircraft => {
  const fuser = new MultiIlluminatorTrackFuser({...config, truth: {adsb: {enabled: aircraft.length > 0, aircraft}}});
  let result;
  for (let index = 0; index < 3; index += 1) {
    const frames = cohort(illuminators, target, index);
    frames.forEach(frame => { frame.adsb = aircraft; });
    result = fuser.update(frames);
    const before = JSON.stringify(result);
    validateFusionAgainstAdsb(result, {aircraft});
    assert.equal(JSON.stringify(result), before, 'Held-out validation cannot mutate solver state');
  }
  return JSON.stringify(result);
});
assert.equal(runs[0], runs[1]); assert.equal(runs[1], runs[2]);
const tracked = JSON.parse(runs[0]);
assert.equal(tracked.tracks.length, 1);
assert.equal(tracked.tracks[0].state, 'FUSED_ACTIVE');
const validation = validateFusionAgainstAdsb(tracked, {aircraft: [
  {hex: 'target', lat: target.latitude, lon: target.longitude, alt_geom: target.altitude / .3048},
  {hex: 'decoy', lat: 41, lon: -81}]});
assert.equal(validation.adsb_used_by_solver, false);
assert.equal(validation.held_out_truth, true);
assert.equal(validation.matches[0].adsb_hex, 'target');
assert.ok(validation.matches[0].error_km < 1e-6);
assert.ok(validation.matches[0].altitude_error_m < .01);

const freshnessFuser = new MultiIlluminatorTrackFuser(config);
for (let index = 0; index < 3; index += 1)
  freshnessFuser.update(cohort(illuminators, target, index));
assert.equal(freshnessFuser.snapshot().operational_localization, true);
const staleSnapshot = freshnessFuser.snapshot(freshnessFuser.lastReceiptAt + 2000);
assert.equal(staleSnapshot.operational_localization, false);
assert.equal(staleSnapshot.tracks[0].state, 'FUSED_COASTING');
const expiredSnapshot = freshnessFuser.snapshot(freshnessFuser.lastReceiptAt + 11000);
assert.equal(expiredSnapshot.operational_localization, false);
assert.equal(expiredSnapshot.tracks.length, 0,
  'Stopped input expires without needing another capture cohort');

for (const mutate of [frames => { frames[2].capture_first_input_sample += 1; },
  frames => { frames[2].capture_epoch += 1; }, frames => { frames[2].timestamp += 1; },
  frames => { frames[2].tracks.timestamp += 1; }, frames => { frames[2].capture_sample_position_available = false; },
  frames => { delete frames[2].capture_epoch; }, frames => { frames[2].id = frames[1].id; },
  frames => { frames[2].id = 'unknown'; },
  frames => { frames.forEach(frame => { frame.capture_end_input_sample = frame.capture_first_input_sample; }); }]) {
  const frames = cohort(); mutate(frames);
  assert.equal(fuseTrackCohort(frames, config).available, false, 'Invalid or misaligned cohorts fail closed');
}
const legacy = cohort();
legacy.forEach(frame => {
  frame.first_input_sample = frame.capture_first_input_sample; frame.end_input_sample = frame.capture_end_input_sample;
  delete frame.capture_sample_position_available; delete frame.capture_epoch;
});
assertDiagnostic(fuseTrackCohort(legacy, config), 'Legacy sample numbers without a capture epoch are diagnostic');
for (const state of ['TENTATIVE', 'ASSOCIATED', 'COASTING']) {
  const frames = cohort(); frames.forEach(frame => { frame.tracks.data[0].state = state; });
  assert.deepEqual(fuseTrackCohort(frames, config).hypotheses, []);
}
const missingRxAltitude = structuredClone(config); delete missingRxAltitude.location.rx.altitude;
assert.equal(fuseTrackCohort(cohort(), missingRxAltitude).available, false);
const missingTxAltitude = structuredClone(config); delete missingTxAltitude.process.multi_illuminator.illuminators[0].transmitter.altitude;
assertDiagnostic(fuseTrackCohort(cohort(), missingTxAltitude), 'Missing altitude cannot become a ground-plane assumption');
const mismatchedCarrier = cohort(); mismatchedCarrier[0].frequency_hz = illuminators[0].frequency_hz + 1;
assertDiagnostic(fuseTrackCohort(mismatchedCarrier, config),
  'A frame cannot relabel Doppler with a carrier outside its configured provenance');
assertDiagnostic(fuseTrackCohort(cohort().slice(0, 2), config), 'Two sites cannot resolve three dimensions');

// Same-site carriers remain one geometric observation, including coordinate
// jitter across the old six-decimal rounding boundary and explicit aliases.
const colocated = [...structuredClone(illuminators), {...structuredClone(illuminators[0]), id: 'east-other'}];
colocated[3].transmitter.latitude += .00003;
colocated[0].transmitter.site_id = 'tower-a'; colocated[3].transmitter.site_id = 'alias-a';
const duplicateResult = fuseTrackCohort(cohort(colocated), configFor(colocated));
assert.equal(duplicateResult.independent_transmitter_sites, 3);
assert.equal(resolved(duplicateResult).length, 1);
assert.equal(resolved(duplicateResult)[0].support, 3);
assert.equal(resolved(duplicateResult)[0].lane_support, 3,
  'One transmitter geometry contributes at most one hit per CPI across carriers');
assert.equal(resolved(duplicateResult)[0].uncertainty.independent_range_count, 3);
assert.equal(duplicateResult.measurement_semantics.geometry_group_vote_limit_per_cpi, 1);
assert.match(duplicateResult.measurement_semantics.false_alarm, /not multiplied/);

// A physical target may be visible on different carriers from one site on
// successive CPIs. Canonical range/range-rate and geometry-group voting keep
// that carrier switching as one hit, retain its source, and continue one fused
// track without treating carrier false alarms as independent confirmations.
const carrierPairs = illuminators.flatMap((definition, siteIndex) => [0, 1].map(carrierIndex => ({
  ...structuredClone(definition), id: `${definition.id}-carrier-${carrierIndex}`,
  frequency_hz: definition.frequency_hz + carrierIndex * 50e6,
  transmitter: {...structuredClone(definition.transmitter), site_id: `physical-site-${siteIndex}`}})));
const switchingFuser = new MultiIlluminatorTrackFuser(configFor(carrierPairs));
let switchingTrackId = null;
for (let sample = 0; sample < 4; sample += 1) {
  const frames = cohort(carrierPairs, target, sample);
  const selectedCarrier = sample % 2;
  frames.forEach((frame, laneIndex) => {
    const definition = carrierPairs[laneIndex];
    frame.frequency_hz = definition.frequency_hz;
    if (laneIndex % 2 !== selectedCarrier) frame.tracks.data = [];
    else {
      const rangeRateKmS = .025;
      frame.tracks.data[0].doppler = -rangeRateKmS * definition.frequency_hz / C;
      frame.tracks.data[0].provenance = {detector: 'lane-cfar', sample};
      frame.tracks.data[0].false_alarm_probability = 1e-6;
    }
  });
  const switching = switchingFuser.update(frames);
  assert.equal(resolved(switching).length, 1);
  const hypothesis = resolved(switching)[0];
  assert.equal(hypothesis.support, 3);
  assert.equal(hypothesis.lane_support, 3);
  assert.deepEqual(hypothesis.contributors.map(item => item.illuminator).sort(),
    carrierPairs.filter((_, index) => index % 2 === selectedCarrier).map(item => item.id).sort());
  for (const contributor of hypothesis.contributors) {
    assert.ok(Math.abs(contributor.bistatic_range_rate_km_s - .025) < 1e-12);
    assert.equal(contributor.physical_delay_definition, 'excess-bistatic-path-km');
    assert.deepEqual(contributor.provenance.track, {detector: 'lane-cfar', sample});
    assert.equal(contributor.source_false_alarm, 1e-6);
  }
  assert.equal(switching.tracks.length, 1);
  switchingTrackId ||= switching.tracks[0].id;
  assert.equal(switching.tracks[0].id, switchingTrackId,
    'Intermittent carrier changes continue the geometry-consistent fused track');
}
const twoSites = [colocated[0], colocated[3], colocated[1]];
const twoSitesResult = fuseTrackCohort(cohort(twoSites), configFor(twoSites));
assert.equal(twoSitesResult.independent_transmitter_sites, 2);
assertDiagnostic(twoSitesResult, 'Coordinate jitter cannot invent a third transmitter site');
const inconsistent = structuredClone(illuminators);
inconsistent[0].transmitter.site_id = inconsistent[1].transmitter.site_id = 'one-tower';
const inconsistentResult = fuseTrackCohort(cohort(inconsistent), configFor(inconsistent));
assertDiagnostic(inconsistentResult, 'A single stable site ID with contradictory coordinates is rejected');
assert.ok(inconsistentResult.diagnostic_reasons.includes('inconsistent-transmitter-site-identity'));

// Raised sites put both mirror solutions inside the aircraft envelope.
const raisedRx = {...receiver, altitude: 5000};
const raisedSites = illuminators.map(item => ({...item, transmitter: {...item.transmitter, altitude: 5000}}));
const mirrorFuser = new MultiIlluminatorTrackFuser(configFor(raisedSites, raisedRx));
let mirrored;
for (let index = 0; index < 5; index += 1) mirrored = mirrorFuser.update(cohort(raisedSites, target, index, raisedRx));
assert.ok(mirrored.hypotheses.filter(item => item.physically_admissible).length >= 2);
assertDiagnostic(mirrored, 'Two physically plausible mirrors remain diagnostic');
assert.equal(mirrored.tracks.length, 0);
assert.ok(mirrored.hypotheses.every(item => item.branch_count >= 2));
// The fourth site is outside the mirrored solution's final 3-sigma gate but
// inside the wider acquisition gate. It cannot erase a valid 3-of-4 branch.
const elevatedFourth = {...illuminators[0], id: 'elevated-south',
  transmitter: {latitude: 39.8, longitude: -80.08, altitude: 12000}};
const mirrorWithFourth = [...raisedSites, elevatedFourth];
const mirrorFourFrames = cohort(mirrorWithFourth, target, 0, raisedRx);
const threeSiteMirror = mirrored.hypotheses.find(item => Math.abs(item.altitude - 3978.96) < 1);
assert.ok(threeSiteMirror, 'The first three sites preserve the exact 3978.96 m mirror');
const mirrorLocal = [threeSiteMirror.local_east_km, threeSiteMirror.local_north_km, threeSiteMirror.local_up_km];
const fourthRange = mirrorFourFrames[3].tracks.data[0].delay * C / elevatedFourth.output_sample_rate_hz;
const fourthSigma = Math.hypot(C / elevatedFourth.output_sample_rate_hz / Math.sqrt(12), .05);
const fourthResidualSigma = Math.abs(excessPath3d(mirrorLocal,
  localPoint3d(raisedRx, elevatedFourth.transmitter)) - fourthRange) / fourthSigma;
assert.ok(Math.abs(fourthResidualSigma - 6.0055) < .001);
const mirrorFourFuser = new MultiIlluminatorTrackFuser(configFor(mirrorWithFourth, raisedRx));
let retainedMirror;
for (let index = 0; index < 4; index += 1)
  retainedMirror = mirrorFourFuser.update(cohort(mirrorWithFourth, target, index, raisedRx));
assert.ok(retainedMirror.hypotheses.some(item => item.support === 3 && Math.abs(item.altitude - 3978.96) < 1));
assert.ok(retainedMirror.hypotheses.some(item => item.support === 4 && Math.abs(item.altitude - 6000) < 1));
assertDiagnostic(retainedMirror, 'Wider acquisition cannot erase the supported 3-site mirror');
assert.equal(retainedMirror.tracks.length, 0, 'Repeated cohorts cannot promote a branch erased by optimization');
const tx = localPoint3d(receiver, illuminators[0].transmitter), original = localPoint3d(receiver, target);
const mirrorNormal = [-tx[1] / Math.hypot(tx[0], tx[1]), tx[0] / Math.hypot(tx[0], tx[1]), 0];
const projected = original.reduce((sum, value, index) => sum + value * mirrorNormal[index], 0);
const secondTarget = geographicPoint3d(receiver, original.map((value, index) => value - 2 * projected * mirrorNormal[index]));
const crossingFrames = cohort(), secondFrames = cohort(illuminators, secondTarget);
for (let index = 1; index < crossingFrames.length; index += 1)
  crossingFrames[index].tracks.data.push({...secondFrames[index].tracks.data[0], id: `other-${index}`});
const crossing = fuseTrackCohort(crossingFrames, config);
assert.ok(new Set(crossing.hypotheses.filter(item => item.physically_admissible).map(item => item.signature)).size >= 2);
assertDiagnostic(crossing, 'Different contributor signatures sharing one observation cannot become simultaneous aircraft');
const equatorRx = {latitude: 0, longitude: 0, altitude: 0};
const collinear = [.1, .2, .3].map((longitude, index) => ({...illuminators[index],
  transmitter: {latitude: 0, longitude, altitude: 0}}));
assertDiagnostic(fuseTrackCohort(cohort(collinear, {latitude: .05, longitude: .15, altitude: 6000}, 0, equatorRx),
  configFor(collinear, equatorRx)), 'Collinear transmitter geometry cannot claim uniqueness');
assertDiagnostic(fuseTrackCohort(cohort(), configFor(illuminators, receiver, {maximumCondition: 1.01})),
  'A failed conditioning threshold prevents promotion');
assertDiagnostic(fuseTrackCohort(cohort(illuminators, {...target, altitude: 1500}), config),
  'An underground mirror whose uncertainty reaches the aircraft envelope still prevents uniqueness');

// Lost support coasts from the last 3-D observation; two-site data never refreshes it.
const motionFuser = new MultiIlluminatorTrackFuser(config);
let moving;
const movingPoint = index => geographicPoint3d(receiver,
  localPoint3d(receiver, target).map((value, axis) => value + (axis === 0 ? index * .1 : 0)));
for (let index = 0; index < 3; index += 1) moving = motionFuser.update(cohort(illuminators, movingPoint(index), index));
const active = moving.tracks[0];
assert.equal(active.state, 'FUSED_ACTIVE');
assert.ok(Math.abs(active.speed_m_s - 100) < .01);
const lost = cohort(illuminators, movingPoint(3), 3); lost[2].tracks.data = [];
const coast = motionFuser.update(lost).tracks[0];
assert.equal(coast.id, active.id); assert.equal(coast.state, 'FUSED_COASTING');
assert.equal(coast.support, 0); assert.deepEqual(coast.contributors, []);
assert.equal(coast.measurement_timestamp, active.measurement_timestamp);
assert.equal(coast.uncertainty.kind, 'propagated');
assert.ok(coast.uncertainty.vertical_sigma_km > active.uncertainty.vertical_sigma_km);
assert.ok(coast.local_east_km > active.local_east_km + .09);
for (let index = 4; index <= 6; index += 1) {
  const frames = cohort(illuminators, movingPoint(index), index); frames.forEach(frame => { frame.tracks.data = []; });
  moving = motionFuser.update(frames);
}
assert.equal(moving.tracks.length, 0, 'Expired coasting tracks are deleted');

const speedFuser = new MultiIlluminatorTrackFuser(config);
let speedResult;
for (let index = 0; index < 3; index += 1) speedResult = speedFuser.update(cohort(illuminators, target, index));
const jump = geographicPoint3d(receiver, localPoint3d(receiver, target).map((value, index) => value + (index === 2 ? 2 : 0)));
const rejectedJump = speedFuser.update(cohort(illuminators, jump, 3));
assert.equal(rejectedJump.tracks.length, 1);
assert.equal(rejectedJump.tracks[0].state, 'FUSED_COASTING');
assert.equal(rejectedJump.operational_localization, false);
assert.ok(rejectedJump.hypotheses.some(item => item.motion_rejected));
assert.ok(rejectedJump.tracks.every(item => item.speed_m_s <= 450));
const resetFrames = cohort(illuminators, target, 0, receiver, 5);
resetFrames.forEach(frame => { frame.timestamp = frame.tracks.timestamp = 100; });
const reset = speedFuser.update(resetFrames);
assert.equal(reset.capture_epoch, 5, 'New capture epochs reset association even if timestamps go backwards');
assert.equal(reset.tracks[0].state, 'FUSED_CANDIDATE');
assert.notEqual(reset.tracks[0].id, speedResult.tracks[0].id);
assert.equal(speedFuser.update(resetFrames), reset, 'Repeating one capture cannot accumulate confirming hits');
assert.equal(speedFuser.update(cohort(illuminators, target, 4)), reset,
  'A delayed cohort from an already retired epoch cannot restore old track identities');

// New presentation timestamps cannot turn previously consumed sample windows
// into an additional confirming observation.
const replayFuser = new MultiIlluminatorTrackFuser(config);
const intervalFrames = (start, end, timestamp, epoch = 4) => {
  const frames = cohort(illuminators, target, 0, receiver, epoch);
  frames.forEach(frame => {
    frame.capture_first_input_sample = start; frame.capture_end_input_sample = end;
    frame.timestamp = frame.tracks.timestamp = timestamp;
  });
  return frames;
};
replayFuser.update(intervalFrames(10000, 11000, 100000));
const beforeReplay = replayFuser.update(intervalFrames(11000, 12000, 101000));
assert.equal(beforeReplay.tracks[0].state, 'FUSED_CANDIDATE');
assert.equal(beforeReplay.tracks[0].supporting_frames, 2);
assert.equal(replayFuser.update(intervalFrames(10000, 11000, 102000)), beforeReplay,
  'A replay of a non-latest sample interval cannot promote the third hit');
assert.equal(replayFuser.update(intervalFrames(9000, 10000, 103000)), beforeReplay,
  'A previously unseen but out-of-order sample interval is rejected');
assert.equal(replayFuser.update(intervalFrames(11500, 12500, 104000)), beforeReplay,
  'An overlapping capture window cannot reuse confirming samples');
let afterLedger;
for (let index = 0; index < 130; index += 1)
  afterLedger = replayFuser.update(intervalFrames(12000 + index * 1000, 13000 + index * 1000, 105000 + index * 1000));
assert.ok(replayFuser.seenCaptureIntervals.size <= 128);
assert.equal(replayFuser.update(intervalFrames(10000, 11000, 300000)), afterLedger,
  'The sample watermark rejects replay even after its interval leaves the bounded ledger');
const freshEpoch = replayFuser.update(intervalFrames(10000, 11000, 100, 5));
assert.equal(freshEpoch.tracks[0].state, 'FUSED_CANDIDATE');
assert.equal(freshEpoch.tracks[0].supporting_frames, 1);
assert.equal(replayFuser.seenCaptureIntervals.size, 1, 'Interval history resets for a new capture epoch');

// Secant velocities are -382.5 and +42.5 m/s over one- and nine-second
// intervals. Their midpoints are five seconds apart: acceleration is 85 m/s².
const irregularFuser = new MultiIlluminatorTrackFuser(config);
let irregular;
for (const [index, seconds, eastKm] of [[0, 0, 0], [1, 1, -.3825], [2, 10, 0]]) {
  const point = geographicPoint3d(receiver, localPoint3d(receiver, target)
    .map((value, axis) => value + (axis === 0 ? eastKm : 0)));
  const frames = cohort(illuminators, point, index);
  frames.forEach(frame => { frame.timestamp = frame.tracks.timestamp = 100000 + seconds * 1000; });
  irregular = irregularFuser.update(frames);
}
assert.equal(irregular.operational_localization, false,
  'Unequal measurement intervals cannot dilute acceleration below the 80 m/s² limit');
assert.equal(irregular.tracks.length, 1);
assert.equal(irregular.tracks[0].supporting_frames, 2);
assert.equal(irregular.tracks[0].measurement_timestamp, 101000);
assert.ok(irregular.hypotheses.some(item => item.motion_rejected));

// Quantized noisy observations exercise an overdetermined fit and uncertainty.
const fourth = {...illuminators[0], id: 'south', transmitter: {latitude: 39.8, longitude: -80.08, altitude: 450}};
const fourSites = [...illuminators, fourth];
let noisyAmbiguousSamples = 0;
for (let sample = 0; sample < 12; sample += 1) {
  const point = {...target, altitude: 3000 + sample * 700};
  const frames = cohort(fourSites, point, sample);
  frames.forEach((frame, index) => {
    frame.tracks.data[0].delay = Math.round(frame.tracks.data[0].delay + Math.sin(sample * 2 + index) * .2);
  });
  const noisy = fuseTrackCohort(frames, configFor(fourSites));
  const solution = noisy.hypotheses.find(item => item.support === 4 && item.physically_admissible);
  assert.ok(solution, `Noisy sample ${sample} retains the overdetermined estimate`);
  assert.equal(solution.support, 4);
  assert.ok(inverse(point, solution).distanceM < 350);
  assert.ok(Math.abs(solution.altitude - point.altitude) < 800);
  assert.ok(Math.abs(solution.altitude - point.altitude) < solution.uncertainty.vertical_sigma_km * 4000);
  assert.equal(solution.uncertainty.calibrated, false);
  if (noisy.hypotheses.filter(item => item.physically_admissible).length > 1) {
    noisyAmbiguousSamples += 1;
    assertDiagnostic(noisy, `Noisy sample ${sample} retains competing supported three-site alternatives`);
  } else assert.equal(resolved(noisy).length, 1);
}
assert.ok(noisyAmbiguousSamples > 0, 'Noise-supported three-site alternatives remain diagnostic');
const crowded = cohort();
crowded.forEach(frame => { frame.tracks.data = Array.from({length: 20}, (_, index) => ({
  ...frame.tracks.data[0], id: `${frame.id}-${index}`, n: index + 1, delay: frame.tracks.data[0].delay + index * .01})); });
const bounded = fuseTrackCohort(crowded, config, {measurementsPerLane: 4, pairEvaluations: 5, hypotheses: 20});
assert.equal(bounded.computation.triple_evaluations, 5);
assert.equal(bounded.computation.truncated, true);
assertDiagnostic(bounded, 'An incomplete bounded search cannot establish uniqueness');
const oneHypothesis = fuseTrackCohort(cohort(), config, {hypotheses: 1});
assert.equal(oneHypothesis.hypotheses.length, 1);
assertDiagnostic(oneHypothesis, 'Dropping the last mirror at the output limit cannot establish uniqueness');

const serverSource = require('fs').readFileSync(require.resolve('./server.js'), 'utf8');
const fusedRoute = serverSource.slice(serverSource.indexOf("app.get('/api/tracker/fused'"),
  serverSource.indexOf("app.get('/api/tracker/native'"));
assert.doesNotMatch(fusedRoute, /multiIlluminatorTrackFuser\.update/,
  'Browser GETs are read-only snapshots, not fusion observations');
const ingestRoute = serverSource.slice(serverSource.indexOf('function acceptIlluminatorFrame'),
  serverSource.indexOf("jsonStreamServer('illuminator'"));
assert.match(ingestRoute,
  /latestMultiIlluminatorFusion = multiIlluminatorTrackFuser\.update\(cohort\)/,
  'Every newly completed capture cohort advances fusion without a browser poll');

console.log('Three-dimensional held-out-truth bistatic localization, uncertainty and adversarial tracking passed.');
