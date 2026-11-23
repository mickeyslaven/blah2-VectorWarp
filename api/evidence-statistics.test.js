'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EvidenceStatistics, createEvidenceStatisticsLanes, associationGates,
  assignments, delayBand, limits, standardizedComparison, siteKey, siteDescriptor,
  provisionalTimingFrame, defaultEvidenceStatisticsPath,
  defaultProvisionalEvidenceStatisticsPath} = require('./evidence-statistics');

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

assert.equal(defaultEvidenceStatisticsPath(config, {VECTORWARP_MACOS_STATE: '/Users/test/state'}),
  '/Users/test/state/evidence-statistics.json');
assert.equal(defaultProvisionalEvidenceStatisticsPath(config,
  {VECTORWARP_MACOS_STATE: '/Users/test/state'}),
  '/Users/test/state/evidence-statistics-provisional.json');
assert.equal(defaultProvisionalEvidenceStatisticsPath(config,
  {BLAH2_STATISTICS_PATH: '/srv/radar/site-scorecard.json'}),
  '/srv/radar/site-scorecard-provisional.json');
assert.equal(defaultProvisionalEvidenceStatisticsPath(config,
  {BLAH2_PROVISIONAL_STATISTICS_PATH: '/custom/operational.json'}),
  '/custom/operational.json');
const prediction = {delay: 10, doppler: 100, timestamp: clock / 1000, flight: 'TEST1',
  doppler_method: 'reported-kinematic', doppler_position_fit_hz: 104,
  doppler_kinematic_hz: 100, doppler_estimator_difference_hz: -4,
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
const nativeReplayCoordinates = {sample_rate_hz: 6144000, frequency_hz: 539e6};
const replayCoordinateConfig = {...config, capture: {...config.capture,
  replay: {state: true, sample_rate_hz: 2e6, frequency_hz: 100e6}},
  process: {...config.process, delay_reference_sample_rate_hz: 2e6}};
require('../test/helpers/trusted-replay-sites')(replayCoordinateConfig);
const nativeReplayLimits = limits(replayCoordinateConfig, nativeReplayCoordinates);
assert.equal(nativeReplayLimits.sampleRate, 6144000);
assert.equal(nativeReplayLimits.frequency, 539e6);
assert.equal(nativeReplayLimits.kmPerBin, 299792.458 / 6144000);
assert.equal(nativeReplayLimits.delayMaxBin, 1536);
assert.equal(limits(replayCoordinateConfig).frequency, 100e6);
assert.equal(limits({...config, capture: {...config.capture, replay: {state: true}}}), null);
for (const bad of [null, false, 0, -1, NaN, Infinity, '6144000'])
  assert.equal(limits(config, {...nativeReplayCoordinates, sample_rate_hz: bad}), null);
assert.equal(limits(config, [nativeReplayCoordinates,
  {...nativeReplayCoordinates, frequency_hz: 100e6}]), null);

const unmappedTiming = {timestamp: clock, captureSamplePositionAvailable: true,
  eventTime: {source: 'usrp_uhd_device_time', exact: false,
    estimateAvailable: false, uncertaintyBounded: false}};
const provisionalTiming = provisionalTimingFrame(unmappedTiming);
assert.equal(provisionalTiming.eventTime.source, 'radar_host_arrival_estimate');
assert.equal(provisionalTiming.eventTime.exact, false);
assert.equal(provisionalTiming.eventTime.estimateAvailable, true);
assert.equal(unmappedTiming.eventTime.estimateAvailable, false,
  'provisional normalization must not mutate exact evidence provenance');
const exactTimingFrame = {...unmappedTiming,
  eventTime: {source: 'usrp_utc', exact: true, estimateAvailable: true}};
assert.equal(provisionalTimingFrame(exactTimingFrame), exactTimingFrame,
  'qualified exact timing must pass through unchanged');

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
  // Native replay coordinates drive both the supplied truth carrier and track
  // bin conversion. The live receiver and saved reference bins deliberately differ.
  let suppliedFrames;
  const nativeStats = new EvidenceStatistics(replayCoordinateConfig, {
    async getAt(_kind, at, frames) { suppliedFrames = frames; return {abc123: {
      ...prediction, timestamp: at / 1000, frequency_hz: 539e6}}; }
  }, {filename: path.join(directory, 'native-replay-coordinates.json'), now: () => clock,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  nativeStats.acceptTracker({...nativeReplayCoordinates, timestamp: clock,
    data: [{id: 'native', state: 'ACTIVE', n: 4, associations: 4,
      delay: 10 / nativeReplayLimits.kmPerBin, doppler: 100}]});
  nativeStats.acceptDetection({...nativeReplayCoordinates, timestamp: clock,
    delay: [10], doppler: [100], snr: [7]});
  nativeStats.acceptTiming({timestamp: clock, eventTime: {exact: true,
    uncertaintyBounded: true, uncertaintyNs: 0}, captureSamplePositionAvailable: true,
    captureEpoch: 12, captureFirstEpochSample: 0, captureEndEpochSample: 3072000});
  while (nativeStats.running) await new Promise(resolve => setImmediate(resolve));
  assert.equal(suppliedFrames.length, 2);
  assert.equal(nativeStats.state.active.abc123.trackedFrames, 1);
  assert.deepEqual(nativeStats.state.active.abc123.observation_coordinates,
    {sample_rate_hz: 6144000, frequency_hz: 539e6});
  nativeStats.adsbSource = {async getAt() { return {abc123: {...prediction, frequency_hz: 100e6}}; }};
  await nativeStats.sample(clock);
  assert.match(nativeStats.state.error, /Truth carrier differs/);
  assert.equal(nativeStats.state.active.abc123.trackedFrames, 1,
    'Wrong-carrier truth must not score or retroactively alter accepted evidence');
  nativeStats.close();
  const lowAngleStats = new EvidenceStatistics(config, source, {
    filename: path.join(directory, 'low-angle-eligibility.json'), now: () => clock,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  lowAngleStats.observe('lowangle', {...prediction,
    geometry: {...prediction.geometry, elevation_deg: .1}}, clock, null, null);
  const lowAnglePass = lowAngleStats.state.active.lowangle;
  assert.equal(lowAnglePass.detectorEligibleFrames, 1,
    'Low elevation alone must never remove a searchable aircraft CPI');
  assert.equal(lowAnglePass.elevationBands['0-2'].eligibleFrames, 1,
    'Low-angle failures must remain visible in the evaluation strata');
  lowAngleStats.close();
  const restartFilename = path.join(directory, 'restart-continuity.json');
  const restartStats = new EvidenceStatistics(config, source, {filename: restartFilename,
    now: () => clock, setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  restartStats.observe('abc123', prediction, clock, null, null);
  restartStats.close();
  let restartState = JSON.parse(fs.readFileSync(restartFilename, 'utf8'));
  assert.equal(restartState.passes.length, 0,
    'API shutdown must not finalize an in-progress pass as a radar miss');
  assert.ok(restartState.active.abc123,
    'API shutdown must persist an in-progress pass for restart continuity');
  const resumedStats = new EvidenceStatistics(config, source, {filename: restartFilename,
    now: () => clock + 1000, setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  assert.ok(resumedStats.state.active.abc123,
    'a brief restart under the same scoring contract must resume the pass');
  resumedStats.state.active.abc123.associationEligibleFrames = 3;
  resumedStats.finish('abc123');
  assert.equal(resumedStats.associationPasses().length, 1);
  resumedStats.state.passes[0].evaluationContractHash = 'previous-scoring-contract';
  assert.equal(resumedStats.associationPasses().length, 0,
    'federation must reject an explicitly different evaluator');
  assert.equal(resumedStats.liveStatus().current_setup_lifetime.passes, 0,
    'Current-setup headlines must reject an explicitly different evaluator too');
  delete resumedStats.state.passes[0].evaluationContractHash;
  resumedStats.state.liveScoreContract.startedAt = clock + 1;
  assert.equal(resumedStats.associationPasses().length, 0,
    'federation must not mix finalized passes from an earlier scoring contract');
  resumedStats.close();
  const endpointFilename = path.join(directory, 'truth-endpoint-retention.json');
  const endpointStats = new EvidenceStatistics(config, source, {filename: endpointFilename,
    now: () => clock, setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  const endpointBase = clock + 100000;
  for (let index = 1; index <= 40; ++index) endpointStats.observe('endpoint', {
    ...prediction, timestamp: (endpointBase + index * 1000) / 1000,
    position: {...prediction.position, altitude_geom_ft: 12000 + index}
  }, endpointBase + index * 1000, null, null);
  endpointStats.observe('endpoint', {
    ...prediction, timestamp: endpointBase / 1000,
    position: {...prediction.position, altitude_geom_ft: 11000},
    geometry: {...prediction.geometry, elevation_deg: 35}
  }, endpointBase, null, null);
  const endpointPass = endpointStats.state.active.endpoint;
  assert.equal(endpointPass.truthHistory.length, 30,
    'The diagnostic truth tail must remain bounded');
  assert.equal(endpointPass.truthHistory[0].timestamp_ms, endpointBase + 11000,
    'Trimming must retain only the newest diagnostic truth samples');
  assert.equal(endpointPass.firstTruth.timestamp_ms, endpointBase,
    'The immutable pass beginning must survive history trimming and late arrival');
  assert.equal(endpointPass.firstTruth.altitude_geom_ft, 11000);
  assert.equal(endpointPass.lastTruth.timestamp_ms, endpointBase + 40000,
    'The immutable pass end must remain the latest observation');
  assert.equal(endpointPass.lastTruth.altitude_geom_ft, 12040);
  assert.equal(endpointPass.elevationBands['30-90'].eligibleFrames, 1,
    'An out-of-order CPI must be stratified by its own geometry, not the latest endpoint');
  endpointStats.close();
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
  assert.equal(result.timing_policy, 'exact');
  assert.equal(result.windows.hour.active_passes, 1);
  assert.equal(result.windows.hour.eligible_passes, 0,
    'An in-progress pass must not be prematurely classified');
  assert.equal(result.windows.hour.live_to_date.eligible_passes, 1,
    'The live hour view must include a currently eligible pass');
  assert.equal(result.windows.hour.live_to_date.detected_passes, 1);
  assert.equal(result.windows.hour.live_to_date.tracked_passes, 1);
  assert.deepEqual(stats.activeAssociations(), [{hex: 'abc123', flight: 'TEST1',
    detected_cpis: 3, tracked_cpis: 3, last_at: clock - 1000}],
  'The map association feed reads bounded active counters without building a scorecard');
  clock += 121000;
  source.get = async () => ({});
  stats.acceptDetection({timestamp: clock, delay: [], doppler: []});
  while (stats.running) await new Promise(resolve => setImmediate(resolve));
  result = stats.status();
  assert.equal(result.windows.hour.eligible_passes, 1);
  assert.equal(result.windows.hour.detected_passes, 1);
  assert.equal(result.windows.hour.tracked_passes, 1);
  assert.equal(result.windows.hour.tracking_yield, 1);
  assert.equal(result.windows.hour.cpi_exposure.eligible_cpis, 3);
  assert.equal(result.windows.hour.cpi_exposure.eligible_seconds, 1.5);
  assert.equal(result.windows.hour.cpi_exposure.detected_percent, 100);
  assert.equal(result.windows.hour.cpi_exposure.missed_detection_percent, 0);
  assert.equal(stats.state.eligibilityEvents.length, 1,
    'A pass must emit only one eligibility event across repeated opportunities');
  assert.equal(result.eligibility_event_count, 1);
  assert.equal(stats.state.eligibilityEvents[0].state, 'finalized');
  assert.equal(stats.state.eligibilityEvents[0].outcome, 'tracked');
  assert.equal(stats.state.eligibilityEvents[0].first_detection_at, 1_800_000_000_000);
  assert.equal(stats.state.eligibilityEvents[0].first_track_at, 1_800_000_000_000);
  assert.equal(trackCaptures.length, 1, 'Only the first matched track frame triggers a capture');
  assert.equal(trackCaptures[0].timestamp, 1_800_000_000_000);
  assert.equal(stats.state.eligibilityEvents[0].predicted.delay_km, 10);
  assert.equal(stats.state.eligibilityEvents[0].truth_samples.length, 3);
  assert.equal(stats.state.eligibilityEvents[0].truth_samples[0].provenance.source_id,
    'local-readsb');
  assert.deepEqual(stats.state.eligibilityEvents[0].truth_samples.map(item => item.timestamp_ms),
    [1_800_000_000_000, 1_800_000_001_000, 1_800_000_002_000]);
  assert.equal(stats.state.eligibilityEvents[0].capture_state, 'not_integrated');
  for (const field of ['receiver_epoch', 'config_epoch', 'site_epoch'])
    assert.match(stats.state.eligibilityEvents[0][field], /^[0-9a-f]{64}$/);
  stats.persist(); // Explicit durability boundary; periodic writes are queued.
  const stored = JSON.parse(fs.readFileSync(filename, 'utf8'));
  assert.equal(stored.schema, 'vectorwarp-evidence-statistics-v2');
  assert.equal(stored.passes[0].firstTruth.altitude_geom_ft, 12000);
  assert.equal(stored.passes[0].firstTruth.doppler_method, 'reported-kinematic');
  assert.equal(stored.passes[0].firstTruth.doppler_estimator_difference_hz, -4);
  assert.equal(stored.passes[0].truthDopplerDiagnostics.comparableFrames, 3);
  assert.equal(stored.passes[0].truthDopplerDiagnostics.maximumAbsDifferenceHz, 4);
  assert.equal(stored.passes[0].truthDopplerDiagnostics.searchableDisagreements, 0);
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
  assert.equal(recovered.state.eligibilityEvents[0].capture_state, 'failed');
  assert.equal(recovered.state.eligibilityEvents[0].capture_failure,
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
  assert.equal(outage.state.eligibilityEvents.length, 1);
  assert.equal(outage.state.eligibilityEvents[0].state, 'pending');
  outageClock += 121000;
  outageSource.get = async () => ({});
  sampler();
  while (outage.running) await new Promise(resolve => setImmediate(resolve));
  outageStatus = outage.status();
  assert.equal(emitted, 1, 'Pass finalization must not re-emit eligibility');
  assert.equal(outage.state.eligibilityEvents[0].state, 'finalized');
  assert.equal(outage.state.eligibilityEvents[0].outcome, 'insufficient_opportunities');
  assert.equal(outage.state.eligibilityEvents[0].eligible_frames, 1);
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
  const retained = retention.state.eligibilityEvents[0].truth_samples;
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
  }}, {filename: path.join(directory, 'uncertain-event-time.json'), now: () => clock,
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
  assert.equal(provisionalStats.status().timing_policy, 'provisional');
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

  // Continuity is a chronological CPI run, not adjacent eligible observations
  // after stale truth, detector exclusions, or an absent radar interval.
  for (const [name, offsets, excluded] of [
    ['normal-cadence', Array.from({length: 40}, (_, index) => index * 500), null],
    ['missing-cadence', [0, 500, 4000, 4500, 12000, 12500], null],
    ['reverse-truth-joins', [1500, 1000, 500, 0], null],
    ['out-of-map', [0, 500, 1000, 1500, 2000], {delay: 1000}],
    ['below-floor', [0, 500, 1000, 1500, 2000], {delay: 0}],
    ['stale-truth', [0, 500, 1000, 1500, 2000], {timestamp: clock / 1000 - 10}],
    ['invalid-truth', [0, 500, 1000, 1500, 2000], {
      provenance: {...prediction.provenance, projection_model: 'nearest_hold'}}]
  ]) {
    const continuityStats = new EvidenceStatistics(config, source, {
      filename: path.join(directory, `continuity-${name}.json`), now: () => clock,
      setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
    for (const offset of offsets) {
      const at = clock + offset, association = {frame_timestamp_ms: at,
        kind: 'detection', cell_index: 0};
      continuityStats.observe('continuity', {...prediction, timestamp: at / 1000,
        ...(offset === 1000 ? excluded : null)}, at, association, association);
    }
    const pass = continuityStats.state.active.continuity;
    const quality = continuityStats.liveStatus().active_passes[0].track_quality;
    const expected = excluded ? 4 : offsets.length;
    assert.deepEqual([pass.eligibleFrames, pass.detectedFrames, pass.trackedFrames],
      [expected, expected, expected], `${name}: continuity must preserve exposure and yield`);
    const longest = name === 'normal-cadence' ? 40 : name === 'reverse-truth-joins' ? 4 : 2;
    assert.equal(quality.longest_consecutive_detected_cpis, longest, name);
    assert.equal(quality.longest_consecutive_operationally_tracked_cpis, longest, name);
    assert.equal(quality.detection_reacquisitions, 0,
      `${name}: an unobserved gap is not an observed miss/reacquisition`);
    assert.equal(quality.continuity_complete_for_pass, true, name);
    assert.ok(pass.qualityCpi.continuity.tail.length <= 32, 'Continuity retention stays bounded');
    if (name === 'normal-cadence') {
      delete pass.qualityCpi.continuity; // State written by the older scoring contract.
      pass.qualityCpi.longestTrackedRun = 999;
      assert.equal(continuityStats.liveStatus().active_passes[0].track_quality
        .longest_consecutive_operationally_tracked_cpis, null,
      'Legacy eligible-observation runs cannot be presented as chronological continuity');
      const at = clock + 20000, association = {frame_timestamp_ms: at, kind: 'detection', cell_index: 0};
      continuityStats.observe('continuity', {...prediction, timestamp: at / 1000}, at,
        association, association);
      const migrated = continuityStats.liveStatus().active_passes[0].track_quality;
      assert.equal(migrated.longest_consecutive_operationally_tracked_cpis, 1);
      assert.equal(migrated.continuity_complete_for_pass, false);
      assert.equal(pass.trackedFrames, 41);
    }
    continuityStats.close();
  }

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
  floorStats.expire(Infinity);
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

  const notchConfig = structuredClone(config);
  notchConfig.process.detection.minDoppler = 15;
  assert.equal(limits(notchConfig).detectionMinAbsDopplerHz, 15);
  assert.notEqual(siteKey(config), siteKey(notchConfig));
  const notchFilename = path.join(directory, 'detector-doppler-notch.json');
  const notchStats = new EvidenceStatistics(notchConfig, source, {
    filename: notchFilename, now: () => clock,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  const notchCases = [
    [-15.001, true, false], [-15, true, false], [-14.999, false, true],
    [0, false, true], [14.999, false, true], [15, true, false], [15.001, true, false],
    [601, false, false], [0, false, false, span.detectionMinDelayKm / 2]];
  for (const [index, [doppler, eligible, excluded, delay = 10]] of notchCases.entries()) {
    const at = clock + index * 500;
    notchStats.observe('abc123', {...prediction, delay, doppler, timestamp: at / 1000},
      at, {frame_timestamp_ms: at, kind: 'detection', cell_index: index}, null);
    const audit = notchStats.state.active.abc123.cpiAudit.at(-1);
    assert.equal(audit.detector_eligible, eligible, `detector mask at ${doppler} Hz`);
    assert.equal(audit.detection_doppler_notch_excluded, excluded,
      `exclusive notch exposure at ${doppler} Hz, delay ${delay}`);
    assert.equal(audit.map_eligible, doppler !== 601,
      'The notch and delay floor must not erase physical map exposure');
  }
  const notchPass = notchStats.state.active.abc123;
  assert.equal(notchPass.physicalEligibleFrames, 8);
  assert.equal(notchPass.detectorEligibleFrames, 4);
  assert.equal(notchPass.eligibleFrames, 4);
  assert.equal(notchPass.detectedFrames, 4,
    'Even a supplied match inside the notch must not become a scored detection');
  assert.equal(notchPass.dopplerNotchFrames, 3);
  assert.equal(notchPass.outsideDopplerFrames, 1);
  notchStats.expire(Infinity);
  notchStats.close();
  assert.equal(notchStats.status().windows.lifetime.out_of_span.inside_detection_doppler_notch, 3);
  for (const minimum of [undefined, 0]) {
    const disabled = structuredClone(config);
    if (minimum !== undefined) disabled.process.detection.minDoppler = minimum;
    assert.equal(limits(disabled).detectionMinAbsDopplerHz, 0);
    const noNotch = new EvidenceStatistics(disabled, source, {
      filename: path.join(directory, `no-notch-${minimum}.json`), now: () => clock,
      setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
    assert.notEqual(noNotch.scoreContractHash, notchStats.scoreContractHash,
      'The evaluation contract must bind the minimum absolute Doppler');
    for (const [index, doppler] of [-.001, 0, .001].entries()) {
      const at = clock + index * 500;
      noNotch.observe('abc123', {...prediction, doppler, timestamp: at / 1000}, at, null, null);
    }
    assert.equal(noNotch.state.active.abc123.eligibleFrames, 3);
    assert.equal(noNotch.state.active.abc123.dopplerNotchFrames, 0);
    noNotch.close();
  }
  // The v4 ledger mixed position-fit/stored-offset evaluation with reported
  // kinematics/aging original observations. Keep its evidence as historical.
  const legacyNotchState = JSON.parse(fs.readFileSync(notchFilename, 'utf8'));
  const legacyKey = 'legacy-v4-truth-policy-site';
  legacyNotchState.sites = {[legacyKey]: {...siteDescriptor(notchConfig),
    detector_eligibility_version: 4}};
  for (const item of legacyNotchState.passes) {
    item.siteKey = legacyKey;
    delete item.evaluationContractHash;
    delete item.dopplerNotchFrames;
  }
  legacyNotchState.rollup = {...legacyNotchState.rollup, siteKey: legacyKey,
    passes: 20, eligible: 17, detected: 6, tracked: 2};
  legacyNotchState.liveScoreContract = {hash: 'legacy-v4-contract', startedAt: clock - 100000};
  const historicalPasses = structuredClone(legacyNotchState.passes);
  const historicalRollup = structuredClone(legacyNotchState.rollup);
  fs.writeFileSync(notchFilename, JSON.stringify(legacyNotchState));
  const migratedNotch = new EvidenceStatistics(notchConfig, source, {
    filename: notchFilename, now: () => clock + 10000,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  assert.deepEqual(migratedNotch.state.passes, historicalPasses,
    'Do not recalculate historical pass counters or rewrite their IDs/site identity');
  assert.deepEqual(migratedNotch.state.sites[legacyKey].finalized_rollup, historicalRollup,
    'Preserve compacted historical counts across the versioned site rollover');
  assert.equal(migratedNotch.associationPasses().length, 0);
  assert.equal(migratedNotch.status().windows.lifetime.eligible_passes, 0);
  assert.equal(migratedNotch.liveStatus().current_setup_lifetime.eligible_passes, 0);
  assert.equal(migratedNotch.measuredTrackBasis().aircraft.length, 0);
  assert.equal(migratedNotch.state.liveScoreContract.startedAt, clock + 10000);
  assert.equal(siteDescriptor(notchConfig).detector_eligibility_version, 6);
  assert.equal(migratedNotch.liveStatus().evaluation_contract.truth_doppler_policy,
    'reported-kinematic-with-position-fit-fallback-v1');
  assert.equal(migratedNotch.liveStatus().evaluation_contract.truth_freshness_policy,
    'original-observation-offset-plus-prediction-age-v1');
  const policyHash = migratedNotch.liveStatus().evaluation_policy_hash;
  assert.match(policyHash, /^[0-9a-f]{64}$/);
  for (const [name, changed, samePolicy] of [
    ['geometry', {...notchConfig, capture: {...notchConfig.capture, fs: 1200000},
      process: {...notchConfig.process, detection: {minDelay: 10, minDoppler: 25}}}, true],
    ['truth-age', {...notchConfig, process: {...notchConfig.process, bearing: {
      ...notchConfig.process.bearing, manifold_recorder: {
        ...notchConfig.process.bearing.manifold_recorder, max_truth_age_s: 2}}}}, false]
  ]) {
    const other = new EvidenceStatistics(changed, source, {
      filename: path.join(directory, `policy-${name}.json`), now: () => clock + 10000,
      setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
    assert.notEqual(other.scoreContractHash, migratedNotch.scoreContractHash);
    assert.equal(other.liveStatus().evaluation_policy_hash === policyHash, samePolicy,
      'Channel geometry may differ; truth/scoring policy must match for additive totals');
    other.close();
  }
  migratedNotch.close();

  const bufferedOptions = {filename: path.join(directory, 'buffered-cohort.json'),
    now: () => clock + 10000, timingPolicy: 'provisional',
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}};
  const buffered = new EvidenceStatistics(config, source, bufferedOptions);
  for (let index = 0; index < 3; ++index) {
    const at = clock + 9000 + index * 500;
    buffered.observe('abc123', {...prediction, timestamp: at / 1000}, at,
      {frame_timestamp_ms: at, cell_index: 0, kind: 'detection'}, null);
  }
  assert.equal(buffered.liveStatus().active_passes.length, 1,
    'A buffered first CPI predating API startup still belongs to its actual evaluator');
  buffered.finish('abc123');
  const bufferedPass = buffered.state.passes[0];
  assert.equal(bufferedPass.evaluationContractHash, buffered.scoreContractHash);
  assert.equal(buffered.isLivePass({...bufferedPass,
    evaluationContractHash: 'previous-policy', startedAt: clock + 11000}), false,
  'A newer event timestamp cannot override an explicit old contract');
  const unknownPass = {...bufferedPass};
  delete unknownPass.evaluationContractHash;
  assert.equal(buffered.isLivePass(unknownPass), false,
    'Do not relabel unversioned historical evidence from before the cohort boundary');
  buffered.close();
  const restoredBuffered = new EvidenceStatistics(config, source, bufferedOptions);
  assert.equal(restoredBuffered.liveStatus().windows.lifetime.eligible_passes, 1);
  assert.equal(restoredBuffered.liveStatus().current_setup_lifetime.detected_passes, 1);
  assert.equal(restoredBuffered.associationPasses().length, 1,
    'Channel totals and deduplicated-pass inputs retain the same buffered cohort after restart');
  assert.equal(restoredBuffered.measuredTrackBasis().aircraft.length, 1);
  Object.assign(restoredBuffered.state.rollup, {passes: 20, eligible: 17,
    associationEligible: 17, detected: 6, tracked: 2});
  restoredBuffered.close();

  const changedTiming = new EvidenceStatistics(config, source,
    {...bufferedOptions, timingPolicy: 'exact'});
  assert.notEqual(changedTiming.key, buffered.key,
    'A contract-only timing change starts a separate site cohort without a config change');
  assert.equal(changedTiming.liveStatus().current_setup_lifetime.eligible_passes, 0);
  assert.equal(changedTiming.associationPasses().length, 0);
  assert.deepEqual(changedTiming.state.passes, restoredBuffered.state.passes,
    'Prior-contract pass identities and counts remain archived');
  assert.deepEqual(changedTiming.state.sites[buffered.key].finalized_rollup,
    restoredBuffered.state.rollup, 'Prior-contract compacted counts remain archived');
  changedTiming.close();

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
  const exactExposure = exact.liveStatus().active_passes[0];
  assert.equal(exactExposure.cpi_exposure.eligible_seconds, 2);
  assert.equal(exactExposure.cpi_exposure.detected_percent, 100);
  assert.equal(exactExposure.cpi_exposure.operationally_tracked_percent, 75);
  assert.equal(exactExposure.track_quality.detected_and_operationally_tracked_cpis, 3);
  assert.equal(exactExposure.track_quality.detected_but_not_operationally_tracked_cpis, 1);
  assert.equal(exactExposure.track_quality.longest_consecutive_operationally_untracked_cpis, 1);
  assert.equal(exactExposure.track_quality.first_operational_track_latency_seconds, .498);
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

  // Native guided ACTIVE is a kinematic prediction, not another measured CPI.
  let guidedNow = exactTimes[0];
  const guidedStats = new EvidenceStatistics(config, {async getAt(_kind, at) {
    return {abc123: {...prediction, timestamp: at / 1000}};
  }}, {filename: path.join(directory, 'guided-provenance.json'), now: () => guidedNow,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  for (let index = 0; index < 3; ++index) {
    guidedNow = exactTimes[0] + index * 500;
    guidedStats.acceptTracker({timestamp: guidedNow, data: [{id: 'guided-track',
      state: 'ACTIVE', measured: false, source: 'guided', n: 100,
      delay: 10 / limits(config).kmPerBin, doppler: 100}]});
    guidedStats.acceptDetection({timestamp: guidedNow, delay: [], doppler: [], snr: []});
    guidedStats.acceptTiming({timestamp: guidedNow, eventTime: {exact: true,
      uncertaintyBounded: true, uncertaintyNs: 0}, captureSamplePositionAvailable: true,
      captureEpoch: 14, captureFirstEpochSample: index * 1200000,
      captureEndEpochSample: (index + 1) * 1200000, captureDroppedSamples: [0, 0, 0, 0, 0]});
    while (guidedStats.running) await new Promise(resolve => setImmediate(resolve));
  }
  assert.equal(guidedStats.state.active.abc123.eligibleFrames, 3);
  assert.equal(guidedStats.state.active.abc123.trackedFrames, 0,
    'Guided ACTIVE predictions must not inflate measured scorecard support');
  guidedStats.close();

  const boundedState = new EvidenceStatistics(config, source, {
    filename: path.join(directory, 'bounded-state.json'),
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  for (let index = 0; index < 501; ++index) {
    const hex = index.toString(16).padStart(6, '0');
    boundedState.state.active[hex] = {id: `pass-${index}`, hex,
      siteKey: boundedState.key, startedAt: index, lastAt: index, frames: 80,
      physicalEligibleFrames: 80, eligibleFrames: 80, detectedFrames: 4,
      trackedFrames: 3, dopplerNotchFrames: 4,
      cpiAudit: Array.from({length: 80}, (_, row) => ({timestamp_ms: row})),
      truthHistory: Array.from({length: 80}, (_, row) => ({timestamp_ms: row}))};
    boundedState.finish(hex);
  }
  assert.equal(boundedState.state.passes.length, 500,
    'Detailed finalized passes must remain bounded for restart-safe persistence');
  assert.equal(boundedState.state.rollup.passes, 1);
  assert.equal(boundedState.state.rollup.tracked, 1);
  assert.equal(boundedState.state.rollup.eligibleCpis, 80);
  assert.equal(boundedState.state.rollup.detectedCpis, 4);
  assert.equal(boundedState.state.rollup.trackedCpis, 3);
  assert.equal(boundedState.summarize(Infinity, clock).cpi_exposure.eligible_cpis, 501 * 80);
  assert.equal(boundedState.summarize(Infinity, clock).cpi_exposure.eligible_seconds,
    501 * 80 * .5);
  assert.equal(boundedState.state.rollup.dopplerNotchFrames, 4);
  assert.equal(boundedState.summarize(Infinity, clock).out_of_span.inside_detection_doppler_notch, 501 * 4,
    'Notch exposure must survive bounded pass-detail eviction');
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
  const witnessFilename = path.join(directory, 'searchable-miss-witnesses.json');
  const witnesses = new EvidenceStatistics(config, source, {
    filename: witnessFilename, timingPolicy: 'provisional', now: () => clock,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  for (const hex of ['miss', 'detected']) {
    for (let index = 0; index < 40; ++index) {
      const at = clock + index * 500;
      const candidate = hex === 'detected' ? {frame_timestamp_ms: at,
        cell_index: 0, kind: 'detection'} : null;
      witnesses.observe(hex, {...prediction,
        timestamp: at / 1000 - (index >= 3 && index % 2 ? 10 : 0),
        delay: index < 3 || index % 2 ? 10 : 1000}, at, candidate, null);
    }
    assert.equal(witnesses.state.active[hex].cpiAudit.length, 16);
    witnesses.finish(hex);
    const pass = witnesses.state.passes.at(-1);
    assert.equal(pass.cpiAudit.length, 4);
    assert.deepEqual(pass.cpiAudit.map(row => row.timestamp_ms),
      [clock, clock + 500, clock + 1000, clock + 19500],
      'Preserve scored witnesses for hits and misses through stale and out-of-window tails');
    assert.deepEqual(pass.cpiAudit.map(row => row.scored), [true, true, true, false]);
    assert.equal(pass.cpiAudit[0].prediction_timestamp_s, clock / 1000);
    assert.equal(pass.cpiAudit[0].observation_time_offset_s, .1);
    assert.equal(pass.cpiAudit[0].doppler_position_fit_hz, 104);
    assert.equal(pass.cpiAudit[0].doppler_kinematic_hz, 100);
    assert.equal(pass.cpiAudit[0].provenance, undefined,
      'Scalar audit witnesses must not duplicate provider metadata or aircraft positions');
    assert.equal(pass.eligibleFrames, 3);
    assert.equal(pass.detectedFrames, hex === 'detected' ? 3 : 0);
  }
  witnesses.close();
  const restoredWitnesses = new EvidenceStatistics(config, source, {
    filename: witnessFilename, timingPolicy: 'provisional', now: () => clock,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  assert.deepEqual(restoredWitnesses.state.passes,
    JSON.parse(JSON.stringify(witnesses.state.passes)),
    'Restart compaction preserves the same bounded diagnostic witnesses');
  restoredWitnesses.close();
  const missingOffsets = new EvidenceStatistics(config, source, {
    filename: path.join(directory, 'missing-truth-offset.json'), now: () => clock,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  for (const [index, offset] of [null, false, undefined, '0'].entries()) {
    const hex = `offset-${index}`;
    missingOffsets.observe(hex, {...prediction, timestamp: clock / 1000,
      provenance: {...prediction.provenance, observation_time_offset_s: offset}}, clock,
    {frame_timestamp_ms: clock, cell_index: 0, kind: 'detection'}, null);
    assert.equal(missingOffsets.state.active[hex].eligibleFrames, 0,
      'Missing or nonnumeric observation offsets cannot coerce into fresh truth');
    assert.equal(missingOffsets.state.active[hex].cpiAudit[0].truth_reason, 'stale-observation');
  }
  missingOffsets.close();
  const uncertaintyConfig = JSON.parse(JSON.stringify(config));
  uncertaintyConfig.truth.adsb.uncertainty_association = {enable: true};
  const uncertaintyStats = new EvidenceStatistics(uncertaintyConfig, source, {
    filename: path.join(directory, 'uncertainty-state.json'), timingPolicy: 'provisional',
    now: () => clock,
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  const uncertaintyPrediction = {...prediction, timestamp: clock / 1000,
    provenance: {...prediction.provenance, observation_time_offset_s: 2.9},
    truth_uncertainty: {valid: true}};
  uncertaintyStats.observe('abc123', uncertaintyPrediction, clock, null, null, null,
    {valid: false, reason: 'uncertainty-exceeds-hard-cap'});
  assert.equal(uncertaintyStats.state.active.abc123.eligibleFrames, 1,
    'over-wide association uncertainty cannot erase fresh aircraft presence');
  assert.equal(uncertaintyStats.state.active.abc123.cpiAudit.at(-1).truth_valid, true);
  assert.equal(uncertaintyStats.state.active.abc123.cpiAudit.at(-1).association_reason,
    'uncertainty-exceeds-hard-cap');
  uncertaintyStats.observe('abc123', {...uncertaintyPrediction, timestamp: (clock + 1000) / 1000},
    clock + 1000, null, null, null,
    {valid: true});
  assert.equal(uncertaintyStats.state.active.abc123.eligibleFrames, 2,
    'opt-in source-age support preserves provisional SDR operation without a hardware clock receipt');
  uncertaintyStats.observe('abc123', {...uncertaintyPrediction,
    timestamp: (clock + 2000) / 1000,
    provenance: {...uncertaintyPrediction.provenance, disagreement: true}}, clock + 2000,
  null, null, null, {valid: true});
  assert.equal(uncertaintyStats.state.active.abc123.eligibleFrames, 2,
    'incompatible source clusters still cannot inflate the accepted denominator');
  for (const [index, reason] of ['truth-not-at-requested-frame',
    'position-accuracy-or-integrity-unavailable'].entries()) {
    const at = clock + 3000 + index * 1000;
    const candidate = {frame_timestamp_ms: at, cell_index: 0, kind: 'detection'};
    uncertaintyStats.observe('abc123', {...uncertaintyPrediction,
      timestamp: (at - (index ? 0 : 50)) / 1000,
      truth_uncertainty: {valid: false, reason}}, at, candidate, candidate, null,
    {valid: false, reason});
    assert.equal(uncertaintyStats.state.active.abc123.cpiAudit.at(-1).association_reason, reason);
  }
  const presencePass = uncertaintyStats.state.active.abc123;
  assert.equal(presencePass.eligibleFrames, 4);
  assert.equal(presencePass.associationEligibleFrames, 1);
  assert.equal(presencePass.associationAbstainedFrames, 3);
  assert.equal(presencePass.detectedFrames, 0, 'presence or abstention cannot manufacture a detection');
  assert.equal(presencePass.trackedFrames, 0, 'presence or abstention cannot manufacture a track');
  const presenceExposure = uncertaintyStats.summarize(Infinity, clock + 4000).cpi_exposure;
  assert.equal(presenceExposure.eligible_cpis, 4);
  assert.equal(presenceExposure.association_abstained_cpis, 3);
  assert.equal(presenceExposure.association_eligible_cpis, 1);
  assert.equal(presenceExposure.detected_cpis, 0);
  assert.equal(presenceExposure.missed_detection_cpis, 1,
    'association abstention is unresolved evidence, never a radar miss');
  assert.equal(presenceExposure.operationally_untracked_cpis, 1);
  assert.equal(presenceExposure.counter_invariants_valid, true);
  assert.equal(uncertaintyStats.liveStatus().active_passes[0].status, 'provisional_association_unresolved',
    'presence-only passes must never enter the live detected/tracked identity feed');
  assert.equal(uncertaintyStats.liveStatus().active_passes[0].track_quality.observed_cpis, 1);
  uncertaintyStats.observe('abc123', uncertaintyPrediction, clock + 5000, null, null, null,
    {valid: false, reason: 'truth-not-at-requested-frame'});
  assert.equal(presencePass.eligibleFrames, 4,
    'cached projections cannot extend the original observation freshness horizon');
  assert.equal(presencePass.cpiAudit.at(-1).truth_reason, 'stale-observation');
  uncertaintyStats.finish('abc123');
  const unresolved = uncertaintyStats.summarize(Infinity, clock + 5000);
  assert.equal(unresolved.eligible_passes, 1);
  assert.equal(unresolved.association_eligible_passes, 0);
  assert.equal(unresolved.association_unresolved_passes, 1);
  assert.equal(unresolved.missed_passes, 0);
  assert.equal(unresolved.eligible_no_radar_return, 0);
  assert.equal(unresolved.detection_yield, null);
  assert.equal(unresolved.tracking_yield, null);
  assert.equal(unresolved.delay_strata.bands[delayBand(uncertaintyStats.limits, 10).id].detection_yield, null);
  assert.equal(uncertaintyStats.liveStatus().recent_finalized_passes[0].outcome, 'association_unresolved');
  assert.equal(uncertaintyStats.associationPasses().length, 0);
  // Eviction must preserve presence versus association coverage in lifetime
  // totals, including an entirely unscorable pass at the archive boundary.
  for (let index = 0; index < 500; ++index) {
    const hex = index.toString(16).padStart(6, '0');
    uncertaintyStats.state.active[hex] = {...presencePass, id: `unresolved-${index}`, hex,
      eligibleFrames: 4, associationEligibleFrames: 0, associationAbstainedFrames: 4};
    uncertaintyStats.finish(hex);
  }
  const archivedUnresolved = uncertaintyStats.summarize(Infinity, clock + 5000);
  assert.equal(archivedUnresolved.eligible_passes, 501);
  assert.equal(archivedUnresolved.association_eligible_passes, 0);
  assert.equal(archivedUnresolved.missed_passes, 0);
  assert.equal(archivedUnresolved.cpi_exposure.association_eligible_cpis, 1);
  assert.equal(archivedUnresolved.cpi_exposure.association_abstained_cpis, 2003);
  assert.equal(archivedUnresolved.cpi_exposure.missed_detection_cpis, 1);
  assert.equal(archivedUnresolved.cpi_exposure.counter_invariants_valid, true);
  uncertaintyStats.close();
  let uncertaintyCaptureRequests = 0;
  const sampledUncertainty = new EvidenceStatistics(uncertaintyConfig, {
    async getAt(_kind, at) { return {abc123: {...prediction, timestamp: at / 1000,
      truth_uncertainty: {schema: 'vectorwarp-truth-uncertainty-v1', valid: true,
        covariance: at < clock + 3000 ? [1, 0, 0, 1e6] : [.001, 0, 0, .1],
        delay_rate_km_s: .1, geometric_doppler_rate_hz_s: 1, doppler_rate_bound_hz_s: 1}}}; }
  }, {filename: path.join(directory, 'sampled-uncertainty-state.json'),
    timingPolicy: 'provisional', now: () => clock,
    onEligibilityEvent() { uncertaintyCaptureRequests += 1; },
    setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  for (let index = 0; index < 4; ++index) {
    const at = clock + index * 1000;
    sampledUncertainty.recentTiming.push({timestamp_ms: at, exact_event_time: false,
      event_time_uncertainty_s: 1, event_time_source: 'radar_host_arrival_estimate'});
    sampledUncertainty.detectionHistory.set(at, {timestamp: at, delay: [10], doppler: [100], snr: [7]});
    sampledUncertainty.trackerHistory.set(at, {timestamp: at, data: [{id: 'candidate',
      state: 'ACTIVE', n: 4, associations: 4,
      delay: 10 / sampledUncertainty.limits.kmPerBin, doppler: 100}]});
    await sampledUncertainty.sample(at);
    assert.equal(sampledUncertainty.state.error, null);
    const pass = sampledUncertainty.state.active.abc123;
    assert.equal(pass.eligibleFrames, index + 1, JSON.stringify(pass.cpiAudit.at(-1)));
    assert.equal(pass.detectedFrames, index === 3 ? 1 : 0);
    assert.equal(pass.trackedFrames, index === 3 ? 1 : 0);
    assert.equal(uncertaintyCaptureRequests, index === 3 ? 1 : 0,
      'presence-only exposure cannot broaden automatic capture admission');
    const exposure = sampledUncertainty.summarize(Infinity, at).cpi_exposure;
    assert.equal(exposure.missed_detection_cpis, 0);
    assert.equal(exposure.operationally_untracked_cpis, 0);
    assert.equal(exposure.detected_percent, index === 3 ? 100 : null);
    assert.equal(exposure.counter_invariants_valid, true);
  }
  assert.equal(sampledUncertainty.state.active.abc123.associationAbstainedFrames, 3);
  sampledUncertainty.close();
  // A capture restart is not a new aircraft pass, but it is a new sensitivity
  // run. Retain operational counts and explicitly exclude mixed/unknown runs.
  const runFile = path.join(directory, 'sample-run-state.json');
  const makeRunStats = () => new EvidenceStatistics(config, source,
    {filename: runFile, timingPolicy: 'provisional', now: () => clock,
      setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
  let runStats = makeRunStats();
  runStats.queue = () => {};
  const runTiming = (at, first, epoch = 0) => {
    runStats.acceptTiming({timestamp: at, captureSamplePositionAvailable: true,
      captureEpoch: epoch, captureFirstEpochSample: first, captureEndEpochSample: first + 1200000,
      eventTime: {exact: false, estimateAvailable: true, source: 'native_sample_clock'}});
    return runStats.recentTiming.at(-1);
  };
  const runObserve = (hex, at, first, epoch = 0) => runStats.observe(hex,
    {...prediction, timestamp: at / 1000}, at, null, null, runTiming(at, first, epoch));
  for (let i = 0; i < 3; i++) runObserve('abc123', clock + i * 500, i * 1200000);
  const firstRun = runStats.state.sensitivityEpoch.id;
  runStats.finish('abc123');
  for (let i = 3; i < 6; i++) runObserve('def456', clock + i * 500, i * 1200000);
  runObserve('def456', clock + 3000, 0);
  const secondRun = runStats.state.sensitivityEpoch.id;
  assert.notEqual(firstRun, secondRun, 'same capture epoch with sample reversal starts a run');
  assert.equal(runStats.state.active.def456.frames, 4, 'restart must not manufacture finalized misses');
  assert.equal(runStats.state.active.def456.sensitivityEpochMixed, true);
  runStats.finish('def456');
  for (let i = 1; i <= 3; i++) runObserve('fedcba', clock + 3000 + i * 500, i * 1200000);
  runStats.finish('fedcba');
  assert.equal(runStats.associationPasses().length, 3, 'all operational history remains available');
  assert.equal(runStats.associationPasses({sensitivityEpochId: secondRun}).length, 1);
  assert.equal(runStats.liveStatus().mix_adjusted_previous_hour_comparison
    .excluded_cross_run_or_unknown_passes, 2);
  assert.equal(runStats.liveStatus().sensitivity_epoch.id, secondRun);
  runTiming(clock + 4500, 3600000); // Exact duplicate must not create a run.
  assert.equal(runStats.state.sensitivityEpoch.id, secondRun);
  runTiming(clock + 1250, 2400000); // Late old-run delivery cannot roll back provenance.
  assert.equal(runStats.state.sensitivityEpoch.id, secondRun);
  assert.equal(runStats.recentTiming.at(-1).sensitivity_epoch_id, null);
  runStats.close();
  runStats = makeRunStats(); runStats.queue = () => {};
  runTiming(clock + 5000, 4800000);
  assert.equal(runStats.state.sensitivityEpoch.id, secondRun,
    'API restart preserves a continuing native sample run');
  runTiming(clock + 5500, 6000000, 1);
  assert.notEqual(runStats.state.sensitivityEpoch.id, secondRun,
    'capture epoch change also starts a new run without requiring external UTC');
  runStats.state.passes[0].sensitivityEpochId = undefined;
  assert.equal(runStats.associationPasses({sensitivityEpochId: firstRun}).length, 0,
    'legacy unknown provenance fails closed for sensitivity selection');
  runStats.close();
  const scanConfig = JSON.parse(JSON.stringify(config));
  scanConfig.capture.aircraft_clips = {unmatched_motion: {enabled: true}};
  const scanStats = new EvidenceStatistics(scanConfig, source, {
    filename: path.join(directory, 'scan-cadence.json'), now: () => clock,
    timingPolicy: 'provisional', setIntervalFn: () => ({unref() {}}),
    clearIntervalFn() {}});
  let populationScans = 0;
  scanStats.trackPopulation.snapshot = () => { populationScans += 1;
    return {capture_candidates: []}; };
  scanStats.emitUnmatchedMotionCapture(clock);
  scanStats.emitUnmatchedMotionCapture(clock + 500);
  scanStats.emitUnmatchedMotionCapture(clock + 4999);
  assert.equal(populationScans, 1,
    'A missing unmatched-motion candidate must not rescan retained tracks every CPI');
  scanStats.emitUnmatchedMotionCapture(clock + 5000);
  assert.equal(populationScans, 2, 'Unmatched-motion candidates remain periodically discoverable');
  scanStats.close();
  fs.rmSync(directory, {recursive: true, force: true});
  console.log('PASS eligibility-filtered persistent evidence statistics');
})().catch(error => { console.error(error); process.exitCode = 1; });
