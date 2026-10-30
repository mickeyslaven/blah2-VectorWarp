'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EvidenceStatistics, createEvidenceStatisticsLanes, associationGates,
  assignments, delayBand, limits, standardizedComparison, siteKey} = require('./evidence-statistics');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-statistics-'));
const filename = path.join(directory, 'statistics.json');
let clock = 1_800_000_000_000;
const config = {capture: {fs: 2400000, fc: 563000000},
  location: {rx: {latitude: 1, longitude: 2}, tx: {latitude: 3, longitude: 4}},
  truth: {adsb: {enabled: true}}, process: {
    ambiguity: {delayMin: -10, delayMax: 500, dopplerMin: -600, dopplerMax: 600},
    clutter: {delayMax: 200}, detection: {minDelay: 5},
    bearing: {channel_mapping: [0, 1], manifold_recorder: {
      max_delay_error_km: .25, max_doppler_error_hz: 5, max_truth_age_s: 3}}}};
const prediction = {delay: 10, doppler: 100, timestamp: clock / 1000, flight: 'TEST1',
  delay_rate_km_s: .1, doppler_rate_hz_s: 7,
  position: {latitude: 1.1, longitude: 2.1, altitude_geom_ft: 12000,
    altitude_baro_ft: 11800, ground_speed_kt: 300, track_deg_true: 90,
    geom_rate_fpm: 500, baro_rate_fpm: 450, category: 'A3', squawk: '1234',
    rssi_dbfs: -18, seen_s: .2, seen_pos_s: .1},
  provenance: {source_id: 'local-readsb', position_age_s: 0,
    observation_time_offset_s: .1, projection_model: 'constant_velocity_ground_track',
    maximum_pairwise_projected_disagreement_km: .1, quality_score: .9,
    agreeing_sources: ['local-readsb'], rejected_sources: [], disagreement: false},
  geometry: {bearing_deg_true: 91, elevation_deg: 12, ground_range_km: 9,
    slant_range_km: 10, transmitter_range_km: 20}};
const source = {async get() { return {abc123: {...prediction, timestamp: clock / 1000}}; }};

const identityConfig = JSON.parse(JSON.stringify(config));
identityConfig.capture.device = {type: 'Kraken', channel_count: 5,
  heimdall: {host: '127.0.0.1', port: 8091, control_port: 8092,
    gain_control: 'measured_per_channel', channel_gains: [8.7, 8.7, 14.4, 3.7, 3.7]},
  array_geometry: {shape: 'regular_polygon', operator_notes: 'installation note'}};
const presentationEdit = JSON.parse(JSON.stringify(identityConfig));
presentationEdit.location.rx.name = 'Short receiver name';
presentationEdit.location.tx.name = 'Short transmitter name';
presentationEdit.capture.device.heimdall.host = 'receiver.local';
presentationEdit.capture.device.heimdall.port = 18091;
presentationEdit.capture.device.array_geometry.operator_notes = 'updated note';
assert.equal(siteKey(identityConfig), siteKey(presentationEdit),
  'labels, connection ports and operator notes must not reset scientific statistics');
const gainEdit = JSON.parse(JSON.stringify(identityConfig));
gainEdit.capture.device.heimdall.channel_gains[2] = 15.7;
assert.notEqual(siteKey(identityConfig), siteKey(gainEdit),
  'applied receiver gain must start a separate statistics identity');
const geometryEdit = JSON.parse(JSON.stringify(identityConfig));
geometryEdit.location.rx.latitude += .001;
assert.notEqual(siteKey(identityConfig), siteKey(geometryEdit),
  'physical site geometry must start a separate statistics identity');

(async () => {
  const orderStats = new EvidenceStatistics(config, source, {
    filename: path.join(directory, 'ordered-live-status.json'), now: () => clock,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  const finalizedPass = (id, lastAt, eligibleFrames) => ({id, hex: id, flight: id,
    siteKey: orderStats.key, startedAt: lastAt - 1000, lastAt, frames: 4,
    eligibleFrames, physicalEligibleFrames: eligibleFrames,
    detectorEligibleFrames: eligibleFrames, detectedFrames: 0, trackedFrames: 0,
    belowFloorFrames: 0, aboveDelayFrames: 0, outsideDopplerFrames: 0,
    staleTruthFrames: 0, missingTimingFrames: 0, outsideClutterFrames: 0,
    delayBands: {}, slantRangeBands: {}, elevationBands: {}, azimuthBands: {},
    azimuthElevationBands: {}, geometryBands: {}});
  orderStats.state.liveScoreContract.startedAt = 1;
  orderStats.state.passes = [finalizedPass('newer', clock - 1000, 3),
    finalizedPass('excluded', clock - 500, 0),
    finalizedPass('older', clock - 2000, 3)];
  const orderedLive = orderStats.liveStatus();
  assert.deepEqual(orderedLive.recent_finalized_passes.map(item => item.flight),
    ['excluded', 'newer', 'older'], 'finalized passes must be ordered by finalization time');
  assert.deepEqual(orderedLive.recent_scored_passes.map(item => item.flight),
    ['newer', 'older'], 'the primary recent list must contain only scored passes');
  orderStats.close();

  const span = limits(config);
  assert.equal(span.delayMaxKm.toFixed(3), '62.457');
  assert.equal(span.detectionMinDelayKm.toFixed(3), '0.625');
  assert.equal(span.dopplerResolutionHz, 2);
  const trackerGate = associationGates(span, prediction, clock, 'tracker');
  assert.equal(trackerGate.dopplerHz, 18.5,
    'Doppler-rate and tracker-lag uncertainty must widen the calibration-only gate');
  assert.ok(trackerGate.delayKm >= span.kmPerBin * 3);
  const uncertainGate = associationGates(span, prediction, clock, 'detection', 1);
  assert.equal(uncertainGate.eventTimeUncertaintyS, 1);
  assert.equal(uncertainGate.dopplerHz, 16.5,
    'A one-second UTC nuisance radius must add one second of measured Doppler motion');
  const trackCaptures = [];
  const stats = new EvidenceStatistics(config, source, {filename, now: () => clock,
    onTrackCapture(event, timestamp) {
      trackCaptures.push({event, timestamp});
      return {accepted: false};
    }});
  for (let index = 0; index < 3; ++index) {
    stats.acceptTracker({timestamp: clock, data: [{id: 'track-1', state: 'ACTIVE', source: 'unassisted',
      confidence: 'primary', n: 7, delay: 10 / span.kmPerBin, doppler: 107}]});
    stats.acceptDetection({timestamp: clock, delay: [10.1], doppler: [102], snr: [8.5]});
    while (stats.running) await new Promise(resolve => setImmediate(resolve));
    clock += 1000;
  }
  let result = stats.status();
  assert.equal(result.windows.hour.active_passes, 1);
  assert.equal(result.windows.hour.eligible_passes, 0,
    'An in-progress pass must not be prematurely classified');
  assert.equal(result.windows.hour.live_to_date.eligible_passes, 1,
    'The live hour view must include a currently eligible pass');
  assert.equal(result.windows.hour.live_to_date.detected_passes, 1);
  assert.equal(result.windows.hour.live_to_date.tracked_passes, 1);
  clock += 121000;
  source.get = async () => ({});
  stats.acceptDetection({timestamp: clock, delay: [], doppler: []});
  while (stats.running) await new Promise(resolve => setImmediate(resolve));
  result = stats.status();
  assert.equal(result.windows.hour.eligible_passes, 1);
  assert.equal(result.windows.hour.detected_passes, 1);
  assert.equal(result.windows.hour.tracked_passes, 1);
  assert.equal(result.windows.hour.tracking_yield, 1);
  assert.equal(result.eligibility_events.length, 1,
    'A pass must emit only one eligibility event across repeated opportunities');
  assert.equal(result.eligibility_event_count, 1);
  assert.equal(result.eligibility_events[0].state, 'finalized');
  assert.equal(result.eligibility_events[0].outcome, 'tracked');
  assert.equal(result.eligibility_events[0].first_detection_at, 1_800_000_000_000);
  assert.equal(result.eligibility_events[0].first_track_at, 1_800_000_000_000);
  assert.equal(trackCaptures.length, 1, 'Only the first matched track frame triggers a capture');
  assert.equal(trackCaptures[0].timestamp, 1_800_000_000_000);
  assert.equal(result.eligibility_events[0].predicted.delay_km, 10);
  assert.equal(result.eligibility_events[0].truth_samples.length, 3);
  assert.equal(result.eligibility_events[0].truth_samples[0].provenance.source_id,
    'local-readsb');
  assert.deepEqual(result.eligibility_events[0].truth_samples.map(item => item.timestamp_ms),
    [1_800_000_000_000, 1_800_000_001_000, 1_800_000_002_000]);
  assert.equal(result.eligibility_events[0].capture_state, 'not_integrated');
  for (const field of ['receiver_epoch', 'config_epoch', 'site_epoch'])
    assert.match(result.eligibility_events[0][field], /^[0-9a-f]{64}$/);
  const stored = JSON.parse(fs.readFileSync(filename, 'utf8'));
  assert.equal(stored.schema, 'vectorwarp-evidence-statistics-v2');
  assert.equal(stored.passes[0].firstTruth.altitude_geom_ft, 12000);
  assert.equal(stored.passes[0].lastTruth.bearing_deg_true, 91);
  assert.equal(stored.passes[0].maxAbsDopplerRateHzS, 7);
  assert.equal(stored.passes[0].azimuthSectors45[2], 3);
  assert.ok(stored.sites[stored.passes[0].siteKey]);
  assert.equal(stored.passes[0].id, stored.eligibilityEvents[0].pass_id);
  assert.equal(stored.passes[0].detectionAssociations[0].measured_snr_db, 8.5);
  assert.equal(stored.passes[0].detectionAssociations[0].delay_residual_km.toFixed(1), '0.1');
  assert.equal(stored.passes[0].trackerAssociations[0].tracker.id, 'track-1');
  assert.equal(stored.passes[0].trackerAssociations[0].measured_delay_km.toFixed(3), '10.000');
  clock += 1000;

  source.get = async () => ({def456: {delay: 70, doppler: 800,
    timestamp: clock / 1000, flight: 'OUT'}});
  for (let index = 0; index < 3; ++index) {
    stats.acceptDetection({timestamp: clock, delay: [], doppler: []});
    while (stats.running) await new Promise(resolve => setImmediate(resolve));
    clock += 1000;
  }
  clock += 121000;
  source.get = async () => ({});
  stats.acceptDetection({timestamp: clock, delay: [], doppler: []});
  while (stats.running) await new Promise(resolve => setImmediate(resolve));
  result = stats.status();
  assert.equal(result.windows.hour.eligible_passes, 1,
    'Out-of-span aircraft must not enter the tracking denominator');
  assert.equal(result.windows.hour.out_of_config_passes, 1);
  assert.equal(result.windows.hour.out_of_span.above_delay_ceiling, 3);
  assert.equal(result.windows.hour.out_of_span.outside_doppler_span, 3);

  // A target which was eligible when capture began must keep exact truth
  // through post-roll after crossing the configured processing boundary.
  clock += 1000;
  source.get = async () => ({ed9e12: {...prediction, delay: 10, doppler: 100,
    timestamp: clock / 1000, flight: 'EDGE'}});
  stats.acceptDetection({timestamp: clock, delay: [], doppler: []});
  while (stats.running) await new Promise(resolve => setImmediate(resolve));
  const edgeEvent = stats.state.eligibilityEvents.at(-1);
  edgeEvent.capture_windows = [{window_start: clock - 2000, window_end: clock + 2000}];
  clock += 500;
  source.get = async () => ({ed9e12: {...prediction, delay: 70, doppler: 700,
    timestamp: clock / 1000, flight: 'EDGE'}});
  stats.acceptDetection({timestamp: clock, delay: [], doppler: []});
  while (stats.running) await new Promise(resolve => setImmediate(resolve));
  const edgeTruthTimes = edgeEvent.truth_samples.slice(-2).map(item => item.timestamp_ms);
  assert.deepEqual(edgeTruthTimes, [clock - 500, clock],
    `Fresh truth must continue through capture post-roll outside the map: ${edgeTruthTimes}`);
  assert.equal(edgeEvent.eligible_frames, 1,
    'Post-roll truth retention must not enlarge the radar eligibility denominator');
  stats.close();
  assert(fs.existsSync(filename));
  const legacy = JSON.parse(fs.readFileSync(filename, 'utf8'));
  legacy.schema = 'vectorwarp-evidence-statistics-v1';
  delete legacy.sites;
  legacy.eligibilityEvents[0].capture_state = 'trigger_pending';
  fs.writeFileSync(filename, JSON.stringify(legacy));
  const recovered = new EvidenceStatistics(config, source, {filename, now: () => clock});
  assert.equal(recovered.status().windows.lifetime.tracked_passes, 1);
  assert.equal(recovered.status().eligibility_events[0].capture_state, 'failed');
  assert.equal(recovered.status().eligibility_events[0].capture_failure,
    'API restarted before capture acknowledgement');
  recovered.close();
  assert.equal(JSON.parse(fs.readFileSync(filename, 'utf8')).schema,
    'vectorwarp-evidence-statistics-v2');

  const outageFilename = path.join(directory, 'detector-outage.json');
  let outageClock = 1_900_000_000_000, sampler, samplerMs, unrefCalled = false;
  let cleared = false, emitted = 0;
  const outageSource = {async get() { return {fedcba: {...prediction,
    timestamp: outageClock / 1000, flight: 'OUTAGE'}}; }};
  const outage = new EvidenceStatistics(config, outageSource, {filename: outageFilename,
    now: () => outageClock,
    setIntervalFn(callback, milliseconds) {
      sampler = callback; samplerMs = milliseconds;
      return {unref() { unrefCalled = true; }};
    },
    clearIntervalFn() { cleared = true; },
    onEligibilityEvent() { emitted += 1; }});
  assert.equal(samplerMs, 500);
  assert.equal(unrefCalled, true, 'The independent sampler must not hold the process open');
  sampler();
  while (outage.running) await new Promise(resolve => setImmediate(resolve));
  let outageStatus = outage.status();
  assert.equal(emitted, 1, 'ADS-B eligibility must emit while the detector is silent');
  assert.equal(outageStatus.eligibility_events.length, 1);
  assert.equal(outageStatus.eligibility_events[0].state, 'pending');
  outageClock += 121000;
  outageSource.get = async () => ({});
  sampler();
  while (outage.running) await new Promise(resolve => setImmediate(resolve));
  outageStatus = outage.status();
  assert.equal(emitted, 1, 'Pass finalization must not re-emit eligibility');
  assert.equal(outageStatus.eligibility_events[0].state, 'finalized');
  assert.equal(outageStatus.eligibility_events[0].outcome, 'insufficient_opportunities');
  assert.equal(outageStatus.eligibility_events[0].eligible_frames, 1);
  assert.equal(outageStatus.windows.lifetime.eligible_passes, 0,
    'One captured opportunity must stay outside the three-frame denominator');
  outage.close();
  assert.equal(cleared, true);
  assert.equal(JSON.parse(fs.readFileSync(outageFilename, 'utf8')).eligibilityEvents.length, 1);

  const retentionFilename = path.join(directory, 'trigger-truth-retention.json');
  let retentionClock = 2_000_000_000_000;
  const retentionSource = {async get() { return {abc123: {...prediction,
    timestamp: retentionClock / 1000}}; }};
  const retention = new EvidenceStatistics(config, retentionSource,
    {filename: retentionFilename, now: () => retentionClock,
      setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  for (let index = 0; index < 65; ++index) {
    retention.acceptDetection({timestamp: retentionClock, delay: [], doppler: []});
    while (retention.running) await new Promise(resolve => setImmediate(resolve));
    retentionClock += 1000;
  }
  const retained = retention.status().eligibility_events[0].truth_samples;
  assert.equal(retained.length, 30);
  assert.deepEqual(retained.map(item => item.timestamp_ms),
    Array.from({length: 30}, (_, index) => 2_000_000_035_000 + index * 1000),
    'The recent trajectory must remain continuous while aggregate pass statistics retain the full outcome');
  retention.close();

  const timingFilename = path.join(directory, 'timing-provenance.json');
  const timingStats = new EvidenceStatistics(config, source, {filename: timingFilename,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  const truthProvenance = {projection_model: 'constant_velocity_ground_track',
    observation_time_offset_s: .2, maximum_pairwise_projected_disagreement_km: .1};
  timingStats.state.eligibilityEvents.push({id: 'timing-event', truth_samples:
    Array.from({length: 6}, (_, index) =>
      ({timestamp_ms: index * 500, provenance: truthProvenance}))});
  for (const [timestamp, first] of [[250, 100], [750, 200], [1250, 300]])
    timingStats.acceptTiming({timestamp, eventTime: {exact: true, uncertaintyBounded: true, uncertaintyNs: 0}, captureSamplePositionAvailable: true,
      captureEpoch: 7, captureFirstEpochSample: first,
      captureEndEpochSample: first + 100, captureDroppedSamples: [0, 0, 0, 0, 0]});
  const timingAdmissions = timingStats.prepareCaptureAdmissions([{eventId: 'timing-event',
    captureWindows: [{captureId: 'timing-clip', revision: 4,
      windowStart: 0, windowEnd: 1500}]}], 4);
  assert.equal(timingAdmissions['timing-clip'].valid, true);
  assert.deepEqual(timingAdmissions['timing-clip'].processor_cpis.map(item =>
    [item.timestamp_ms, item.epoch, item.first_epoch_sample, item.end_epoch_sample]),
  [[250, 7, 100, 200], [750, 7, 200, 300], [1250, 7, 300, 400]]);
  assert.deepEqual(timingAdmissions['timing-clip'].cpis.map(item => item.timestamp_ms),
    [250, 750, 1250]);
  timingStats.close();

  let uncertainTruthCalls = 0;
  const uncertainStats = new EvidenceStatistics(config, {getAt() {
    uncertainTruthCalls += 1; return Promise.resolve({});
  }}, {filename: path.join(directory, 'uncertain-event-time.json'),
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  uncertainStats.acceptTiming({timestamp: 1500, captureSamplePositionAvailable: true,
    captureEpoch: 1, captureFirstEpochSample: 0, captureEndEpochSample: 1200000,
    eventTime: {source: 'kraken_heimdall_host_arrival_estimate', exact: false,
      uncertaintyBounded: false, uncertaintyNs: null,
      reason: 'Heimdall V2 has no device timestamp'}});
  assert.equal(uncertainTruthCalls, 0,
    'inexact Kraken host-arrival time must not request exact ADS-B truth');
  const uncertainStatus = uncertainStats.liveStatus();
  assert.deepEqual(uncertainStatus.current_setup_lifetime,
    uncertainStats.status().windows.lifetime,
    'Live scorecard must retain the persistent current-setup lifetime beyond its bounded contract window');
  assert.equal(uncertainStatus.decision_grade, false,
    'an exact-policy lane without exact RF time must fail closed');
  assert.match(uncertainStatus.warning, /blocked.*exact RF event time/i);
  assert.equal(uncertainStatus.data_quality.event_time_rejections, 1);
  assert.equal(uncertainStatus.data_quality.last_event_time_rejection.source,
    'kraken_heimdall_host_arrival_estimate');
  uncertainStats.close();

  let provisionalTruthCalls = 0;
  const provisionalStats = new EvidenceStatistics(config, {getAt() {
    provisionalTruthCalls += 1; return Promise.resolve({abc123: {
      ...prediction, timestamp: 1.5, delay: 10, doppler: 100,
      provenance: {...prediction.provenance, observation_time_offset_s: 0}}});
  }}, {filename: path.join(directory, 'provisional-event-time.json'),
    timingPolicy: 'provisional',
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  // This cell is outside the ordinary detection gate (9.5 Hz here), but
  // inside the declared one-second timing tube (16.5 Hz). It proves the
  // nuisance allowance affects validation association only.
  provisionalStats.acceptDetection({timestamp: 1500, delay: [10],
    doppler: [112], snr: [5]});
  provisionalStats.acceptTiming({timestamp: 1500, captureSamplePositionAvailable: true,
    captureEpoch: 1, captureFirstEpochSample: 0, captureEndEpochSample: 1200000,
    eventTime: {source: 'kraken_heimdall_host_arrival_estimate', exact: false,
      estimateAvailable: true, uncertaintyBounded: false, uncertaintyNs: null,
      reason: 'Heimdall V2 has no device timestamp'}});
  while (provisionalStats.running) await new Promise(resolve => setImmediate(resolve));
  assert.equal(provisionalTruthCalls, 1,
    'provisional lane must retain operational association without claiming exact time');
  const provisionalStatus = provisionalStats.liveStatus();
  assert.equal(provisionalStatus.decision_grade, false);
  assert.equal(provisionalStatus.timing_policy, 'provisional');
  assert.equal(provisionalStatus.provisional_event_time_uncertainty_s, 1);
  assert.equal(provisionalStatus.data_quality.nuisance_accounting,
    'bounded-event-time-rate-expanded-association-gates');
  assert.equal(provisionalStats.state.active.abc123.detectedFrames, 1,
    'bounded timing uncertainty must retain a physically rate-consistent echo association');
  assert.equal(provisionalStats.state.active.abc123.detectionAssociations[0]
    .event_time_uncertainty_s, 1);
  assert.match(provisionalStatus.warning, /inexact receiver time estimate/);
  assert.equal(provisionalStatus.data_quality.event_time_rejections, 1,
    'provisional use must retain the explicit exact-time rejection evidence');
  provisionalStats.close();

  for (const [receiver, timingSource, exactTime] of [
    ['Kraken', 'kraken_heimdall_host_arrival_estimate', false],
    ['RspDuo', 'rspduo_host_arrival_estimate', false],
    ['HackRF', 'hackrf_host_arrival_estimate', false],
    ['Usrp', 'usrp_device_time_without_utc', false],
    ['Usrp', 'usrp_device_time_utc', true]
  ]) {
    let captures = 0;
    const dependencies = {now: () => 1500,
      setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}};
    const lanes = createEvidenceStatisticsLanes({...config,
      capture: {...config.capture, device: {type: receiver}}}, {
      getAt(_kind, at) { return Promise.resolve({abc123: {...prediction,
        timestamp: at / 1000, provenance: {...prediction.provenance,
          observation_time_offset_s: 0}}}); }
    }, {
      exact: {...dependencies, filename: path.join(directory, `${timingSource}-exact.json`),
        onEligibilityEvent() { throw new Error('Exact scorecard must not duplicate captures'); }},
      operational: {...dependencies, filename: path.join(directory, `${timingSource}-operational.json`),
        onEligibilityEvent() { ++captures; return {accepted: true}; }}
    });
    for (const lane of Object.values(lanes)) {
      lane.acceptDetection({timestamp: 1500, delay: [10], doppler: [100], snr: [5]});
      lane.acceptTiming({timestamp: 1500, captureSamplePositionAvailable: true,
        captureEpoch: 1, captureFirstEpochSample: 0, captureEndEpochSample: 1200000,
        eventTime: {source: timingSource, exact: exactTime, estimateAvailable: true,
          uncertaintyBounded: exactTime, uncertaintyNs: exactTime ? 0 : null}});
    }
    while (Object.values(lanes).some(lane => lane.running))
      await new Promise(resolve => setImmediate(resolve));
    assert.equal(captures, 1, `${receiver}: timing capability must leave exactly one capture owner`);
    assert.equal(lanes.exact.liveStatus().decision_grade, exactTime);
    assert.equal(lanes.operational.state.active.abc123.detectedFrames, 1);
    const event = lanes.operational.state.eligibilityEvents[0];
    const admission = lanes.operational.prepareCaptureAdmissions([{eventId: event.id,
      captureWindows: [{captureId: 'operational-clip', revision: 1,
        windowStart: 1400, windowEnd: 1600}]}], 1)['operational-clip'];
    assert.equal(admission.valid, true, `${receiver}: provisional timing must retain capture admission`);
    assert.equal(admission.decision_grade, exactTime);
    assert.equal(admission.timing_policy, exactTime ? 'exact' : 'provisional');
    assert.equal(admission.event_time_uncertainty_s, exactTime ? 0 : 1);
    Object.values(lanes).forEach(lane => lane.close());
  }

  const ambiguityDiagnostics = {};
  const ambiguous = assignments([{index: 0, rawDelay: 10, delay: 10, doppler: 100}],
    [['abc123', prediction], ['def456', {...prediction, delay: 10.01}]],
    () => ({delayKm: 1, dopplerHz: 25, eventTimeUncertaintyS: 0}),
    'detection', clock, ambiguityDiagnostics);
  assert.equal(ambiguous.size, 0, 'Near-tied aircraft must not receive invented associations');
  assert.equal(ambiguityDiagnostics.ambiguous_points, 1,
    'Discarded near-tied detections must remain visible in scorecard diagnostics');

  const floorStats = new EvidenceStatistics(config, source, {
    filename: path.join(directory, 'detector-floor.json'), now: () => clock,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  const belowFloor = {...prediction, delay: span.detectionMinDelayKm / 2,
    timestamp: clock / 1000};
  floorStats.observe('abc123', belowFloor, clock, null, null);
  assert.equal(floorStats.state.active.abc123.physicalEligibleFrames, 1,
    'A below-floor point is still inside the computed ambiguity map');
  assert.equal(floorStats.state.active.abc123.detectorEligibleFrames, 0);
  assert.equal(floorStats.state.active.abc123.eligibleFrames, 0,
    'A point intentionally suppressed by minDelay must never count as a radar miss');
  const inBand = {...prediction, delay: span.detectionMinDelayKm * 2};
  for (let index = 1; index <= 3; ++index)
    floorStats.observe('abc123', {...inBand, timestamp: (clock + index * 500) / 1000},
      clock + index * 500, {frame_timestamp_ms: clock + index * 500,
        kind: 'detection', cell_index: index}, null);
  assert.equal(floorStats.state.active.abc123.detectorEligibleFrames, 3);
  assert.equal(floorStats.state.active.abc123.eligibleFrames, 3);
  assert.equal(floorStats.state.active.abc123.maxSlantRangeKm, 10);
  assert.equal(floorStats.state.active.abc123.maxTransmitterRangeKm, 20);
  assert.equal(floorStats.state.active.abc123.maxBistaticRangeProductKm2, 200);
  floorStats.close();
  const floorStatus = floorStats.status().windows.lifetime;
  assert.equal(floorStatus.map_eligible_passes, 1);
  assert.equal(floorStatus.detector_eligible_passes, 1);
  assert.equal(floorStatus.eligible_passes, 1);
  const floorBand = delayBand(span, inBand.delay);
  assert.equal(floorStatus.delay_strata.bands[floorBand.id].eligible_passes, 1);
  assert.equal(floorStatus.delay_strata.bands[floorBand.id].detected_passes, 1);
  assert.equal(floorStatus.receiver_slant_range_strata.bands['10-20'].eligible_passes, 1,
    'Scorecard must stratify the denominator by 3-D receiver slant range');
  assert.equal(floorStatus.elevation_strata.bands['10-15'].eligible_passes, 1,
    'Scorecard must stratify the denominator by geometric elevation');
  assert.equal(floorStatus.azimuth_strata.bands['90-135'].detected_passes, 1,
    'Scorecard must stratify detections by true azimuth');
  assert.equal(floorStatus.azimuth_elevation_strata.bands['90-135|10-15'].tracked_passes, 0,
    'Joint angle strata must preserve detection and tracking as separate outcomes');
  assert.equal(floorStats.liveStatus().adsb_quality.local_only, true);
  assert.match(floorStats.liveStatus().adsb_quality.warning, /Local-only ADS-B/);

  const pass = (band, detected) => ({geometryBands: {[band]: {eligibleFrames: 10}},
    detectedFrames: detected ? 3 : 0, trackedFrames: detected ? 3 : 0});
  const mixAdjusted = standardizedComparison(
    [pass('near', true), pass('far', false), pass('far', false), pass('far', false)],
    [pass('near', true), pass('near', true), pass('near', true), pass('far', false)], 1);
  assert.equal(mixAdjusted.current.detection_yield, mixAdjusted.baseline.detection_yield,
    'Direct standardization must not call a changed near/far traffic mix a regression');
  assert.equal(mixAdjusted.common_support_coverage, 1);

  // Synthetic delayed truth: all CPI identities must survive concurrent four-CPI
  // association waits. Resolve them out of order to catch accidental latest-frame joins.
  const exactFilename = path.join(directory, 'exact-delayed-cpi.json');
  const delayed = new Map();
  let exactNow = 2_100_000_000_000;
  const exactSource = {getAt(_kind, targetAt) {
    return new Promise(resolve => delayed.set(targetAt, resolve));
  }};
  const exact = new EvidenceStatistics(config, exactSource, {filename: exactFilename,
    now: () => exactNow,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  const exactTimes = Array.from({length: 4}, (_, index) => 2_100_000_000_000 + index * 500);
  for (const [index, timestamp] of exactTimes.entries()) {
    exact.acceptTracker({timestamp, data: [{id: 'exact-track', state: 'ACTIVE',
      n: index ? 4 : 2, delay: 10 / limits(config).kmPerBin, doppler: 100}]});
    exact.acceptDetection({timestamp, delay: [10], doppler: [100], snr: [7]});
    exact.acceptTiming({timestamp: timestamp + 2, eventTime: {exact: true, uncertaintyBounded: true, uncertaintyNs: 0}, captureSamplePositionAvailable: true,
      captureEpoch: 12, captureFirstEpochSample: index * 1200000,
      captureEndEpochSample: (index + 1) * 1200000, captureDroppedSamples: [0, 0, 0, 0, 0]});
  }
  assert.equal(exact.running, 4, 'The association wait must retain every CPI, not only the newest');
  for (const timestamp of [...exactTimes].reverse()) delayed.get(timestamp + 2)({abc123: {
    ...prediction, timestamp: (timestamp + 2) / 1000,
    provenance: {...prediction.provenance, observation_time_offset_s: .2}}});
  while (exact.running) await new Promise(resolve => setImmediate(resolve));
  const exactPass = exact.state.active.abc123;
  assert.equal(exactPass.eligibleFrames, 4);
  assert.equal(exactPass.detectedFrames, 4);
  assert.equal(exactPass.trackedFrames, 3,
    'Tentative/under-supported tracker cells must not receive stable-track credit');
  assert.deepEqual(exactPass.cpiAudit.map(item => item.timestamp_ms),
    exactTimes.map(timestamp => timestamp + 2));
  assert.deepEqual(exactPass.cpiAudit.map(item => item.timing.epoch), [12, 12, 12, 12]);
  assert.equal(exact.liveStatus().data_quality.missing_timing_cpis, 0);

  const invalidAt = exactTimes.at(-1) + 500;
  exact.acceptDetection({timestamp: invalidAt, delay: [10], doppler: [100]});
  exact.acceptTiming({timestamp: invalidAt + 2, eventTime: {exact: true, uncertaintyBounded: true, uncertaintyNs: 0}, captureSamplePositionAvailable: true,
    captureEpoch: 12, captureFirstEpochSample: 4800000,
    captureEndEpochSample: 6000000, captureDroppedSamples: [0, 0, 0, 0, 0]});
  delayed.get(invalidAt + 2)({abc123: {...prediction, timestamp: (invalidAt + 2) / 1000,
    provenance: {...prediction.provenance, projection_model: 'nearest_hold'}}});
  while (exact.running) await new Promise(resolve => setImmediate(resolve));
  assert.equal(exactPass.eligibleFrames, 4,
    'Held/fallback truth must be audited but never scored as a radar miss');
  assert.equal(exactPass.cpiAudit.at(-1).truth_reason, 'invalid-projection');
  exact.close();

  const boundedState = new EvidenceStatistics(config, source, {
    filename: path.join(directory, 'bounded-state.json'),
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  for (let index = 0; index < 501; ++index) {
    const hex = index.toString(16).padStart(6, '0');
    boundedState.state.active[hex] = {id: `pass-${index}`, hex,
      siteKey: boundedState.key, startedAt: index, lastAt: index, frames: 80,
      physicalEligibleFrames: 80, eligibleFrames: 80, detectedFrames: 4,
      trackedFrames: 3,
      cpiAudit: Array.from({length: 80}, (_, row) => ({timestamp_ms: row})),
      truthHistory: Array.from({length: 80}, (_, row) => ({timestamp_ms: row}))};
    boundedState.finish(hex);
  }
  assert.equal(boundedState.state.passes.length, 500,
    'Detailed finalized passes must remain bounded for restart-safe persistence');
  assert.equal(boundedState.state.rollup.passes, 1);
  assert.equal(boundedState.state.rollup.tracked, 1);
  assert.equal(boundedState.state.passes.at(-1).cpiAudit.length, 4);
  assert.equal(boundedState.state.passes.at(-1).truthHistory.length, 0);
  boundedState.state.active.abcdef = {id: 'active-pass', hex: 'abcdef',
    siteKey: boundedState.key, startedAt: 1, lastAt: 2, frames: 1000,
    sampledBuckets: Array.from({length: 1000}, (_, row) => row),
    cpiAudit: Array.from({length: 1000}, (_, row) => ({timestamp_ms: row,
      timing: {epoch: 1, first_epoch_sample: row * 1200000}}))};
  boundedState.state.eligibilityEvents.push({id: 'active-event', state: 'pending',
    truth_samples: Array.from({length: 1000}, (_, row) => ({timestamp_ms: row,
      position: {latitude: 1, longitude: 2}})), capture_windows: [{
      capture_id: 'clip-1', truth_samples: Array.from({length: 12}, (_, row) =>
        ({timestamp_ms: row}))}]});
  boundedState.persist();
  assert.equal(boundedState.state.active.abcdef.cpiAudit.length, 16,
    'Active engineering detail must not make persistence block the API event loop');
  assert.equal(boundedState.state.active.abcdef.sampledBuckets.length, 16);
  assert.equal(boundedState.state.eligibilityEvents.at(-1).truth_samples.length, 30);
  assert.equal(boundedState.state.eligibilityEvents.at(-1).capture_windows[0].truth_samples.length, 12,
    'Compaction must preserve the exact truth bracket copied into a recording window');
  assert.ok(fs.statSync(path.join(directory, 'bounded-state.json')).size < 8 * 1024 * 1024,
    'Bounded synthetic state must remain well below the loader safety cap');
  boundedState.close();
  fs.rmSync(directory, {recursive: true, force: true});
  console.log('PASS eligibility-filtered persistent evidence statistics');
})().catch(error => { console.error(error); process.exitCode = 1; });
