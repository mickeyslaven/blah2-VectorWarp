'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {EvidenceStatistics, associationGates, limits} = require('./evidence-statistics');

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
  provenance: {source_id: 'local-readsb', position_age_s: .1, quality_score: .9,
    agreeing_sources: ['local-readsb'], rejected_sources: [], disagreement: false},
  geometry: {bearing_deg_true: 91, elevation_deg: 12, ground_range_km: 9,
    slant_range_km: 10, transmitter_range_km: 20}};
const source = {async get() { return {abc123: {...prediction, timestamp: clock / 1000}}; }};

(async () => {
  const span = limits(config);
  assert.equal(span.delayMaxKm.toFixed(3), '62.457');
  assert.equal(span.detectionMinDelayKm.toFixed(3), '0.625');
  assert.equal(span.dopplerResolutionHz, 2);
  const trackerGate = associationGates(span, prediction, clock, 'tracker');
  assert.equal(trackerGate.dopplerHz, 18.5,
    'Doppler-rate and tracker-lag uncertainty must widen the calibration-only gate');
  assert.ok(trackerGate.delayKm >= span.kmPerBin * 3);
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
  assert.equal(retained.length, 65);
  assert.deepEqual(retained.map(item => item.timestamp_ms),
    Array.from({length: 65}, (_, index) => 2_000_000_000_000 + index * 1000),
    'The retained trajectory must remain continuous without a minute-wide middle gap');
  retention.close();
  fs.rmSync(directory, {recursive: true, force: true});
  console.log('PASS eligibility-filtered persistent evidence statistics');
})().catch(error => { console.error(error); process.exitCode = 1; });
