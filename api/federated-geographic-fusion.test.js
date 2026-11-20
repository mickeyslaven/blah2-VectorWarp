'use strict';

const assert = require('node:assert/strict');
const {FederatedGeographicFusion} = require('./federated-geographic-fusion');
const {localPoint3d, excessPath3d} = require('./multi-illuminator-track-fusion');
const {channelObservation, compactTrack, buildObservationBundle} = require('./radar-observation-bundle');

const C = 299792.458;
const receiver = {latitude: 40, longitude: -80, altitude: 250};
const transmitters = [
  {latitude: 40, longitude: -79.75, altitude: 400},
  {latitude: 40.22, longitude: -80, altitude: 500},
  {latitude: 40.02, longitude: -80.22, altitude: 300}
];
const target = {latitude: 40.11, longitude: -79.91, altitude: 6000};
const config = {location: {rx: receiver}, process: {data: {cpi: .5},
  tracker: {initiate: {M: 3, N: 6, maxAcc: 10}, delete: 3},
  multi_illuminator: {enabled: true, illuminators: [], co_location_radius_m: 100}}};

assert.equal(channelObservation({nodeId: 'hp', serverSessionId: 'session',
  configRevision: 'revision', id: 'primary', receiver, transmitter: transmitters[0],
  frequencyHz: 500e6, sampleRateHz: 2.4e6, cpiSeconds: .5,
  frame: JSON.stringify({timestamp: 1, eventTime: {epoch: 'e', firstSample: '0', endSample: '1'},
    data: []})}).source_key, 'hp:primary', 'Native publication strings are normalized atomically');

const primaryConfig = {capture: {fs: 2400000, fc: 204640000},
  process: {data: {cpi: .5}, delay_reference_sample_rate_hz: 2000000},
  location: {rx: receiver, tx: transmitters[0]}};
const primaryFrame = {timestamp: 1, sample_rate_hz: 2400000, frequency_hz: 204640000,
  capture_sample_position_available: true,
  capture_epoch: 9007199254740992, capture_first_input_sample: 9007199254740992,
  capture_end_input_sample: 9007199255740992,
  input_lineage: {available: true, sampleInterval: 'half-open',
    captureEpochExact: '9007199254740993', firstInputSampleExact: '9007199254740993',
    endInputSampleExact: '9007199255740993'},
  data: [{id: 'target', state: 'ACTIVE', delay: 480, doppler: -200}]};
const primaryBundle = (settings = primaryConfig, frame = primaryFrame) => buildObservationBundle({
  nodeId: 'kraken', serverSessionId: 'session', configRevision: 'revision',
  config: settings, primaryTracks: frame});
const primaryObservation = primaryBundle().channels[0];
assert.equal(primaryObservation.delay_reference_sample_rate_hz, 2400000,
  'Native tracker bins are not saved 2MHz configuration coordinates');
assert.ok(Math.abs(primaryObservation.tracks.data[0].delay * C /
  primaryObservation.delay_reference_sample_rate_hz - 400 * C / 2000000) < 1e-10,
  'Kraken range must not be inflated by twenty percent');
assert.equal(primaryObservation.lineage.epoch, '9007199254740993');
assert.equal(primaryObservation.lineage.first_sample, '9007199254740993');
assert.equal(primaryObservation.lineage.end_sample, '9007199255740993');
const replayConfig = structuredClone(primaryConfig);
replayConfig.capture.replay = {state: true, sample_rate_hz: 2000000, frequency_hz: 100000000};
const replayFrame = {...primaryFrame, sample_rate_hz: 6144000, frequency_hz: 539000000};
assert.equal(primaryBundle(replayConfig, JSON.stringify(replayFrame)).channels.length, 0,
  'Replay geographic observations require authenticated recorded sites');
require('../test/helpers/trusted-replay-sites')(replayConfig);
const replayObservation = primaryBundle(replayConfig, JSON.stringify(replayFrame)).channels[0];
assert.equal(replayObservation.delay_reference_sample_rate_hz, 6144000);
assert.equal(replayObservation.frequency_hz, 539000000,
  'Recorded native frame carrier wins over stale live/configured replay carrier');
for (const value of [null, 0, -1, true, Infinity, NaN]) {
  assert.equal(primaryBundle(primaryConfig, {...primaryFrame, sample_rate_hz: value}).channels.length, 0);
  assert.equal(primaryBundle(primaryConfig, {...primaryFrame, frequency_hz: value}).channels.length, 0);
}
const legacyFrame = {...primaryFrame}; delete legacyFrame.input_lineage;
assert.equal(primaryBundle(primaryConfig, legacyFrame).channels[0].lineage.first_sample, null,
  'Rounded compatibility integers cannot masquerade as exact sample lineage');
for (const value of [-1, null, true, NaN, Infinity])
  assert.equal(compactTrack({...primaryFrame.data[0], delay_sigma_bins: value}), null,
    'Invalid uncertainty must not disappear and become a precise observation');

function channel(node, id, transmitter, timestamp, index) {
  const sampleRate = 2.4e6, local = localPoint3d(receiver, target);
  const excess = excessPath3d(local, localPoint3d(receiver, transmitter));
  return {id, source_key: `${node}:${id}`, node_id: node, receiver, transmitter,
    frequency_hz: 500e6, delay_reference_sample_rate_hz: sampleRate, timestamp,
    cpi_seconds: .5,
    measurement_time: {basis: 'producer-bounded-event-time', uncertainty_ms: 250},
    altitude_datum: 'WGS84-ellipsoid',
    delay_units: 'bins-at-delay-reference-sample-rate', doppler_units: 'hz-positive-closing',
    lineage: {epoch: 'epoch-1', first_sample: String(index * 1000),
      end_sample: String((index + 1) * 1000), frame_timestamp: timestamp},
    tracks: {timestamp, data: [{id: `${id}-track`, state: 'ACTIVE',
      delay: excess / (C / sampleRate), doppler: 0, n: 8}]}};
}
function bundles(index) {
  const localAt = 100000 + index * 1000, remoteAt = localAt + 200;
  return [
    {schema: 'vectorwarp-radar-observation-bundle-v1', node_id: 'home',
      server_session_id: 'home-session', config_revision: 'home-config',
      generated_at: localAt + 100, federation_local: true,
      federation_transport: {request_started_at: localAt + 190, received_at: localAt + 210}, channels: [
        channel('home', 'east', transmitters[0], localAt, index),
        channel('home', 'north', transmitters[1], localAt, index),
        channel('home', 'west', transmitters[2], localAt, index)]},
    {schema: 'vectorwarp-radar-observation-bundle-v1', node_id: 'hp',
      server_session_id: 'hp-session', config_revision: 'hp-config',
      generated_at: remoteAt + 100, federation_local: false,
      federation_transport: {request_started_at: localAt + 190, received_at: localAt + 210}, channels: [
        channel('hp', 'east-copy', transmitters[0], remoteAt, index)]}
  ];
}

const fusion = new FederatedGeographicFusion(config);
let result;
for (let index = 1; index <= 3; index += 1)
  result = fusion.update(bundles(index), 100000 + index * 1000 + 210);
assert.equal(result.available, true);
assert.equal(result.independent_transmitter_sites, 3,
  'A duplicate transmitter observed by HP is corroboration, not a fourth geometry equation');
assert.equal(result.federation.source_count, 4);
assert.ok(result.tracks.some(track => track.state === 'FUSED_ACTIVE'));
const support = result.tracks.find(track => track.state === 'FUSED_ACTIVE').support;
assert.equal(support, 3);

const beforeKey = result.key;
const beforeSupportingFrames = result.tracks.find(track => track.state === 'FUSED_ACTIVE').supporting_frames;
const repeated = fusion.update(bundles(3), 103210);
assert.equal(repeated.key, beforeKey);
assert.equal(repeated.tracks.find(track => track.state === 'FUSED_ACTIVE').supporting_frames,
  beforeSupportingFrames,
  'Polling the same per-source evidence cannot increase temporal support');

const farReceiver = structuredClone(bundles(4));
farReceiver[1].channels[0].receiver = {...receiver, latitude: receiver.latitude + .01};
const isolated = new FederatedGeographicFusion(config).update(farReceiver, 104210);
assert.equal(isolated.available, true, 'Three valid local sources remain usable');
assert.ok(isolated.federation.rejected_sources.some(item =>
  item.reason === 'distinct-receiver-geographic-solver-not-qualified'));

const relabelled = structuredClone(bundles(3));
for (const bundle of relabelled) {
  bundle.generated_at += 500;
  bundle.federation_transport.request_started_at += 500;
  bundle.federation_transport.received_at += 500;
  for (const item of bundle.channels) {
    item.timestamp += 500; item.tracks.timestamp += 500;
  }
}
const relabelledResult = fusion.update(relabelled, 103710);
assert.equal(relabelledResult.key, beforeKey);
assert.equal(relabelledResult.tracks.find(track => track.state === 'FUSED_ACTIVE').supporting_frames,
  beforeSupportingFrames,
  'Presentation relabelling cannot make the same sample intervals vote again');

const missingLineage = structuredClone(bundles(1));
missingLineage[1].channels[0].lineage = {};
const missingResult = new FederatedGeographicFusion(config).update(missingLineage, 101210);
assert.ok(missingResult.federation.rejected_sources.some(item => item.reason === 'missing-source-lineage'));

const wrongUnits = structuredClone(bundles(1));
wrongUnits[1].channels[0].doppler_units = 'm/s';
const unitResult = new FederatedGeographicFusion(config).update(wrongUnits, 101210);
assert.ok(unitResult.federation.rejected_sources.some(item => item.reason === 'invalid-channel-observation'));

const stale = structuredClone(bundles(1));
stale[1].generated_at += 20000;
const staleResult = new FederatedGeographicFusion(config).update(stale, 101210);
assert.ok(staleResult.federation.rejected_sources.some(item =>
  item.reason === 'stale-or-future-source-observation'));

const frozenFusion = new FederatedGeographicFusion(config);
frozenFusion.update(bundles(1), 101210);
const mixed = bundles(2); mixed[1] = bundles(1)[1];
const advancedLocal = frozenFusion.update(mixed, 102210);
assert.equal(advancedLocal.available, true);
assert.equal(advancedLocal.federation.external_source_contributing, false,
  'A frozen optional node cannot stall or replace fresh local fusion');

const overlappingFusion = new FederatedGeographicFusion(config);
overlappingFusion.update(bundles(1), 101210);
const overlapping = bundles(2);
overlapping[1].channels[0].lineage.first_sample = '1500';
const overlapResult = overlappingFusion.update(overlapping, 102210);
assert.ok(overlapResult.federation.rejected_sources.some(item =>
  item.reason === 'overlapping-or-replayed-source-interval'));

const retiredFusion = new FederatedGeographicFusion(config);
retiredFusion.update(bundles(1), 101210);
const newSession = bundles(2); newSession[1].server_session_id = 'hp-session-2';
const afterSessionChange = retiredFusion.update(newSession, 102210);
const supportAfterSessionChange = Math.max(...afterSessionChange.tracks.map(item => item.supporting_frames));
const oldSession = bundles(3);
const retiredResult = retiredFusion.update(oldSession, 103210);
assert.ok(retiredResult.federation.rejected_sources.some(item => item.reason === 'retired-source-domain'));
assert.ok(Math.max(...retiredResult.tracks.map(item => item.supporting_frames)) > supportAfterSessionChange,
  'Rejected old remote domains cannot reset healthy local temporal support');

const emptyRemoteFusion = new FederatedGeographicFusion(config);
let emptyRemoteResult;
for (let index = 1; index <= 3; index += 1) {
  const values = bundles(index); values[1].channels[0].tracks.data = [];
  emptyRemoteResult = emptyRemoteFusion.update(values, 100000 + index * 1000 + 210);
}
assert.equal(emptyRemoteResult.federation.external_source_contributing, false,
  'An empty remote lane cannot replace authoritative local fusion');

const unalignedEpochFusion = new FederatedGeographicFusion(config);
unalignedEpochFusion.update(bundles(1), 101210);
const alignedSecond = unalignedEpochFusion.update(bundles(2), 102210);
const supportBeforeUnaligned = Math.max(...alignedSecond.tracks.map(item => item.supporting_frames));
const unalignedNewEpoch = bundles(3);
unalignedNewEpoch[1].channels[0].lineage.epoch = 'epoch-unconsumed';
unalignedNewEpoch[1].federation_transport.request_started_at -= 2000;
unalignedNewEpoch[1].federation_transport.received_at -= 2000;
const afterUnaligned = unalignedEpochFusion.update(unalignedNewEpoch, 103210);
assert.ok(Math.max(...afterUnaligned.tracks.map(item => item.supporting_frames)) > supportBeforeUnaligned,
  'An out-of-window new remote epoch cannot reset advancing local support');
const returnedCurrentEpoch = unalignedEpochFusion.update(bundles(4), 104210);
assert.ok(Math.max(...returnedCurrentEpoch.tracks.map(item => item.supporting_frames)) >= 3,
  'Returning current remote domain remains compatible after an unconsumed epoch');

const twoSiteFusion = new FederatedGeographicFusion(config);
let twoSiteResult;
for (let index = 1; index <= 3; index += 1) {
  const values = bundles(index);
  values[0].channels = values[0].channels.slice(0, 2);
  twoSiteResult = twoSiteFusion.update(values, 100000 + index * 1000 + 210);
}
assert.ok(twoSiteResult.position_candidates.some(item => item.geometric_support === 2 &&
  item.corroborating_contributors?.some(source => source.illuminator === 'hp:east-copy')),
  'HP duplicate corroborates a two-site envelope without inventing a third geometry');
assert.equal(twoSiteResult.federation.external_source_contributing, true,
  'Scientifically used remote corroboration selects the federated view');

const unboundedTimingFusion = new FederatedGeographicFusion(config);
let unboundedTimingResult;
for (let index = 1; index <= 3; index += 1) {
  const values = bundles(index);
  for (const bundle of values) for (const item of bundle.channels)
    item.measurement_time = {basis: 'producer-unbounded-event-time', uncertainty_ms: null};
  unboundedTimingResult = unboundedTimingFusion.update(values, 100000 + index * 1000 + 210);
}
assert.equal(unboundedTimingResult.operational_localization, false);
assert.deepEqual(unboundedTimingResult.tracks, []);
assert.ok(unboundedTimingResult.hypotheses.every(item => item.resolved === false));

const timingTransitionFusion = new FederatedGeographicFusion(config);
let qualifiedResult;
for (let index = 1; index <= 3; index += 1)
  qualifiedResult = timingTransitionFusion.update(bundles(index), 100000 + index * 1000 + 210);
assert.ok(qualifiedResult.tracks.some(item => item.state === 'FUSED_ACTIVE'));
const unboundedTransition = bundles(4);
for (const bundle of unboundedTransition) for (const item of bundle.channels)
  item.measurement_time = {basis: 'producer-unbounded-event-time', uncertainty_ms: null};
const transitionDiagnostic = timingTransitionFusion.update(unboundedTransition, 104210);
assert.deepEqual(transitionDiagnostic.tracks, []);
assert.deepEqual(timingTransitionFusion.snapshot(104220).tracks, [],
  'A bounded-to-unbounded transition cannot republish prior operational tracks');

const consumedTargetFusion = new FederatedGeographicFusion(config);
consumedTargetFusion.sourceWatermarks.set('hp:east-copy', {session: 'hp-session',
  revision: 'hp-config', epoch: 'epoch-1', end_sample: '2000'});
const consumedTargetValues = bundles(1);
consumedTargetValues[0].federation_transport.request_started_at = 101100;
consumedTargetValues[0].federation_transport.received_at = 101300;
for (const item of consumedTargetValues[0].channels)
  item.measurement_time.uncertainty_ms = item.id === 'north' ? 300 : 500;
const delayed = consumedTargetValues[0].channels.find(item => item.id === 'north');
delayed.timestamp -= 450; delayed.tracks.timestamp -= 450;
const excludedPreliminaryTarget = consumedTargetValues[1].channels[0];
excludedPreliminaryTarget.timestamp += 100; excludedPreliminaryTarget.tracks.timestamp += 100;
const consumedTargetResult = consumedTargetFusion.update(consumedTargetValues, 101210);
assert.equal(consumedTargetResult.available, true, JSON.stringify(consumedTargetResult));
assert.ok(!consumedTargetResult.lanes.includes('home:north'),
  'Alignment is requalified after an excluded preliminary target changes the target bound');

for (const badCase of ['empty', 'too-few', 'invalid-uncertainty', 'stale', 'out-of-order']) {
  const guarded = new FederatedGeographicFusion(config);
  let active;
  for (let index = 1; index <= 3; index += 1) active = guarded.update(bundles(index), 100000 + index * 1000 + 210);
  assert.ok(active.tracks.some(item => item.state === 'FUSED_ACTIVE'));
  let values = bundles(4), now = 104210;
  if (badCase === 'empty') values = [];
  if (badCase === 'too-few') { values = [values[0]]; values[0].channels.length = 1; }
  if (badCase === 'invalid-uncertainty')
    for (const bundle of values) for (const item of bundle.channels) item.tracks.data[0].delay_sigma_bins = -1;
  if (badCase === 'stale') now += 30000;
  if (badCase === 'out-of-order') {
    values = bundles(2);
    for (const bundle of values) for (const item of bundle.channels) {
      item.lineage.first_sample = '4000'; item.lineage.end_sample = '5000';
    }
  }
  const unavailable = guarded.update(values, now);
  assert.equal(unavailable.available, false, badCase);
  assert.equal(unavailable.operational_localization, false, badCase);
  assert.deepEqual(unavailable.tracks, [], badCase);
  assert.deepEqual(guarded.snapshot(now).tracks, [], `${badCase}: snapshot cannot revive prior operational result`);
  const recovered = guarded.update(bundles(5), 105210);
  assert.equal(recovered.available, true, `${badCase}: fresh valid observations recover`);
  assert.ok(recovered.tracks.some(item => item.state === 'FUSED_ACTIVE'), badCase);
}

console.log('Federated fusion alignment, lineage, freshness and geometry contracts passed.');
