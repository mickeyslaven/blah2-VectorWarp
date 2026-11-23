'use strict';

const assert = require('node:assert/strict');
const {ingestTrackerFrame} = require('./tracker-ingestion');

const legacy = {timestamp: 1000, n: 2, nTentative: 1, nAssociated: 0,
  nActive: 1, nConfirmed: 0, nCoasting: 0, data: []};
const calls = [];
const health = {accept(frame, options) { calls.push(['health', frame, options]); return true; }};
const exact = {acceptTracker(frame) { calls.push(['exact', frame]); }};
const provisional = {acceptTracker(frame) { calls.push(['provisional', frame]); }};
const oprs = {acceptTracker(frame) { calls.push(['oprs', frame]); }};

// Legacy primary products still feed health and the three ordinary evidence
// consumers, but cannot fabricate causal diagnostics from absent metadata.
const legacyResult = ingestTrackerFrame({tracker: legacy, radarPerformanceHealth: health,
  evidenceConsumers: [exact, provisional, oprs], resetOnTimestampRegression: true});
assert.deepEqual(legacyResult, {healthAccepted: true, causalAccepted: false});
assert.deepEqual(calls, [
  ['health', legacy, {channel: 'primary', diagnostics: null, resetOnTimestampRegression: true}],
  ['exact', legacy], ['provisional', legacy], ['oprs', legacy]
]);

// A canonical single-lane product may carry the same diagnostics contract as
// an illuminator envelope. Admission forwards that producer evidence exactly;
// it never derives or fills counters in the API.
calls.length = 0;
const primaryDiagnostics = {detections: 2, birth_candidates: 1,
  birth_snr_rejected: 0, birth_ridge_rejected: 0, initiated: 1,
  associations: 1, promotions: 1, misses: 0, tentative_expired: 0,
  inactivity_deleted: 0, birth_exclusive_rejected: 1};
ingestTrackerFrame({tracker: legacy, diagnostics: primaryDiagnostics,
  radarPerformanceHealth: health});
assert.deepEqual(calls, [['health', legacy,
  {channel: 'primary', diagnostics: primaryDiagnostics,
    resetOnTimestampRegression: false}]]);

calls.length = 0;
const envelope = {id: 'fm-90.5', timestamp: 2000, frequency_hz: 90500000,
  sample_rate_hz: 300000, first_input_sample: 100, end_input_sample: 200,
  detections: {delay: [], doppler: [], snr: []}, tracks: {...legacy, timestamp: 2000},
  trackerDiagnostics: {detections: 0, tracks_before: 1, tracks_after: 2,
    candidate_track_pairs: 0, pair_already_claimed: 0, pair_delay_rejected: 0,
    pair_doppler_rejected: 0, pair_in_gate: 0, associations: 1, misses: 0,
    promotions: 0, tentative_expired: 0, inactivity_deleted: 0,
    birth_candidates: 1, birth_snr_rejected: 1, birth_ridge_rejected: 0, initiated: 0}};
const recorder = {status() { return {api_ingest_enabled: true}; }, accept(frame) {
  calls.push(['causal', frame]); return true;
}};
const multiEvidence = {accept(frame) { calls.push(['multi', frame]); }};
const multiResult = ingestTrackerFrame({tracker: envelope.tracks, channel: envelope.id,
  diagnostics: envelope.trackerDiagnostics, radarPerformanceHealth: health,
  multiIlluminatorEvidence: multiEvidence, causalFunnelRecorder: recorder,
  causalFrame: envelope});
assert.deepEqual(multiResult, {healthAccepted: true, causalAccepted: true});
assert.equal(calls[0][0], 'causal');
assert.equal(calls[0][1].channel, 'fm-90.5');
assert.deepEqual(calls[1], ['health', envelope.tracks,
  {channel: 'fm-90.5', diagnostics: envelope.trackerDiagnostics, resetOnTimestampRegression: false}]);
assert.deepEqual(calls[2], ['multi', envelope]);

console.log('Unified legacy and multi-illuminator tracker ingestion passed.');

// Native primary and illuminator transports retain the same immutable plan in
// their tracker product. Reporting must not invent a route-specific horizon.
const {RadarPerformanceHealth} = require('./radar-performance-health');
const nativeHealth = new RadarPerformanceHealth();
for (let index = 1; index <= 25; ++index) {
  const native = {...legacy, timestamp: index * 500,
    resolved_product_plan: JSON.stringify({schema: 'vectorwarp-resolved-products-v1', trackerN: 25})};
  ingestTrackerFrame({tracker: JSON.stringify(native), radarPerformanceHealth: nativeHealth});
  ingestTrackerFrame({tracker: native, channel: 'native-lane', radarPerformanceHealth: nativeHealth,
    causalFrame: {...envelope, tracks: native}});
}
const nativeChannels = nativeHealth.snapshot().channels;
assert.deepEqual(nativeChannels.map(channel => channel.samples), [25, 25]);
assert.deepEqual(nativeChannels[0].observationWindow, nativeChannels[1].observationWindow);
