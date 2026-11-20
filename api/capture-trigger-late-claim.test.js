'use strict';

const assert = require('node:assert/strict');
const {CaptureTriggerQueue, optionsFromEnvironment,
  NATIVE_RING_SECONDS, CLAIM_CONTROL_MARGIN_MS, CLAIM_RING_GUARD_MS} =
  require('./capture-trigger-queue');
const {EvidenceStatistics} = require('./evidence-statistics');
const {MultiIlluminatorEvidence} = require('./multi-illuminator-evidence');

const config = {capture: {aircraft_clips: {enabled: true,
  pre_seconds: 2, post_seconds: 2}}, process: {data: {cpi: .5}},
  truth: {adsb: {association_lag_cpis: 4}}};
const options = optionsFromEnvironment({}, config);
assert.equal(options.lateClaimAllowanceMs, 2000,
  'HP: configured 2s association lag + fixed 2s margin - 2s post-roll');
assert.equal(CLAIM_CONTROL_MARGIN_MS, 2000);
assert.equal(CLAIM_RING_GUARD_MS, 1000);
assert.equal(NATIVE_RING_SECONDS, 10);
for (const lag of [-1, 9, NaN, 1.5]) assert.throws(() =>
  optionsFromEnvironment({}, {...config, truth: {adsb: {association_lag_cpis: lag}}}),
  /association lag/);
for (const cpi of [0, -1, NaN, Infinity]) assert.throws(() =>
  optionsFromEnvironment({}, {...config, process: {data: {cpi}}}), /association lag/);
for (const allowance of [-1, Infinity, 10001]) assert.throws(() =>
  new CaptureTriggerQueue({lateClaimAllowanceMs: allowance}), /native ring/);
assert.equal(optionsFromEnvironment({}, {...config, process: {data: {cpi: 10}},
  truth: {adsb: {association_lag_cpis: 8}}}).lateClaimAllowanceMs, 10000);

const epoch = 'a'.repeat(64);
const event = (id, at = 100000) => ({id: 'eligibility:' + id, pass_id: 'pass-' + id,
  state: 'pending', observed_at: at, hex: 'abc123', flight: 'TEST',
  predicted: {delay_km: 5, doppler_hz: 100, slant_range_km: 5, elevation_deg: 5},
  receiver_epoch: epoch, config_epoch: epoch, site_epoch: epoch});
let clock = 90000;
const queue = new CaptureTriggerQueue({...options, now: () => clock,
  minimumExposureIntervalMs: 0});
const parent = event('late', clock);
queue.acceptEligibilityEvent(parent);
queue.request(true);
queue.acknowledge(1, null, {file: '/synthetic/initial', samples: 1});
clock = 102900; // event/track CPI is 2.9s old; post-roll already ended.
const accepted = queue.acceptTrackSnapshot(parent, 100000);
const original = {...accepted.clip};
let terminalCalls = 0;
const claimed = queue.requestWithEvidence(() => ++terminalCalls);
assert.equal(claimed.pending, true);
assert.equal(claimed.clipRevision, 2);
for (const key of ['requestedEventTime', 'windowStart', 'windowEnd'])
  assert.equal(claimed[key], original[key], 'late admission cannot recenter RF');
assert.deepEqual(claimed.captureIds, original.captureIds);
assert.deepEqual(claimed.eventIds, original.eventIds);
assert.equal(queue.requestWithEvidence(() => ++terminalCalls).clipRevision, 2);
assert.equal(terminalCalls, 0, 'a late valid window is not a terminal failure');
assert.equal(queue.acceptTrackSnapshot(parent, 100500).reason, 'track-cap');
// Native is authoritative: a real sample-ring miss is terminal and never
// retried as a different, fresh RF window.
queue.acknowledge(2, 'Kraken clip pre-roll is unavailable');
assert.equal(queue.retryPreRollFailure(2, clock), null);

const guard = new CaptureTriggerQueue({preSeconds: 2, postSeconds: 2,
  lateClaimAllowanceMs: 10000, now: () => clock});
guard.acceptEligibilityEvent(event('guard'));
assert.equal(guard.claimDeadline(guard.current()), 107000,
  'oldest requested sample must fit in the 10s ring minus 1s guard');
clock = 107001;
assert.equal(guard.request(true).failure, 'capture-window-expired-before-recorder');

// Exercise the real evidence update path without any filesystem or timer.
function evidenceFor(id) {
  const result = Object.create(EvidenceStatistics.prototype);
  result.state = {active: {}, passes: [], eligibilityEvents: [{
    id, pass_id: 'pass-lane', state: 'pending', capture_windows: []}]};
  result.now = () => clock;
  result.writes = 0;
  result.persist = () => ++result.writes;
  return result;
}
const lane = evidenceFor('eligibility:lane-local');
const other = evidenceFor('eligibility:other');
const owner = Object.create(MultiIlluminatorEvidence.prototype);
owner.lanes = new Map([['a', {operational: lane}], ['b', {operational: other}]]);
const expired = new CaptureTriggerQueue({...options, now: () => clock,
  minimumExposureIntervalMs: 0});
const laneEvent = {...event('lane'), illuminator: 'a', lane_event_id: 'eligibility:lane-local'};
expired.acceptEligibilityEvent(laneEvent);
owner.acceptCaptureEvents([...expired.events.values()]);
assert.equal(lane.state.eligibilityEvents[0].capture_windows[0].state, 'pending');
lane.writes = 0;
expired.requestWithEvidence(records => owner.acceptCaptureEvents(records));
const failed = lane.state.eligibilityEvents[0].capture_windows[0];
assert.equal(failed.state, 'failed');
assert.equal(failed.failure, 'capture-window-expired-before-recorder');
assert.equal(failed.admission_state, 'not-recorded');
assert.deepEqual(failed.admission_reasons, ['capture-window-expired-before-recorder']);
assert.equal(failed.window_start, 98000);
assert.equal(failed.window_end, 102000);
assert.equal(failed.file, '');
assert.equal(lane.writes, 1);
assert.equal(other.writes, 0, 'terminal state must never reach another illuminator');
expired.requestWithEvidence(records => owner.acceptCaptureEvents(records));
assert.equal(lane.writes, 1, 'polling must not duplicate a terminal update');

// A failing evidence sink leaves the bounded notification retryable.
const retry = new CaptureTriggerQueue({now: () => clock});
retry.acceptEligibilityEvent(event('retry'));
assert.throws(() => retry.requestWithEvidence(() => { throw new Error('sink unavailable'); }),
  /sink unavailable/);
let retried = 0;
retry.requestWithEvidence(records => { assert.equal(records.length, 1); ++retried; });
retry.requestWithEvidence(() => ++retried);
assert.equal(retried, 1);

// Even an immediate request() failure after expiry cannot skip reconciliation.
const immediate = new CaptureTriggerQueue({now: () => clock});
immediate.acceptEligibilityEvent(event('immediate'));
const current = immediate.current.bind(immediate);
let currentCalls = 0, reconciled = 0;
immediate.current = () => {
  if (++currentCalls === 2) throw new Error('request failed after terminal transition');
  return current();
};
assert.throws(() => immediate.requestWithEvidence(records => {
  assert.equal(records[0].captureWindows[0].state, 'failed');
  ++reconciled;
}), /request failed/);
assert.equal(reconciled, 1);
immediate.current = current;
immediate.requestWithEvidence(() => ++reconciled);
assert.equal(reconciled, 1);

// The server binds both synchronous expiry and native errors to the same
// single/multi-lane owner; one shared readiness/storage gate owns every trigger.
const source = require('node:fs').readFileSync(require.resolve('./server'), 'utf8');
assert.match(source, /requestWithEvidence\(records =>\s*captureAdmissionEvidence\.acceptCaptureEvents\(records\)/);
assert.doesNotMatch(source, /captureEvidenceStatistics\.acceptCaptureEvents\(/);
assert.equal((source.match(/function admitAutomaticCapture\(/g) || []).length, 1);
assert.equal((source.match(/!recordingLibrary\.canRecord\(\)\.allowed/g) || []).length, 1);
assert.equal((source.match(/=> admitAutomaticCapture\(/g) || []).length, 4);
console.log('bounded late capture claims, terminal reconciliation and lane ownership passed');
