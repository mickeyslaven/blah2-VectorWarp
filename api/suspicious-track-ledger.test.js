'use strict';

const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {SuspiciousTrackLedger} = require('./suspicious-track-ledger');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'suspicious-ledger-'));
const filename = path.join(directory, 'ledger.json');
const definitions = [
  {id: 'a', frequency_hz: 509_000_000, output_sample_rate_hz: 1_000_000,
    transmitter: {latitude: 40, longitude: -80, altitude: 300}},
  {id: 'b', frequency_hz: 515_000_000, output_sample_rate_hz: 1_000_000,
    transmitter: {latitude: 40, longitude: -80, altitude: 300}},
  {id: 'c', frequency_hz: 527_000_000, output_sample_rate_hz: 1_000_000,
    transmitter: {latitude: 40.2, longitude: -79.8, altitude: 320}},
];
let now = 1000;
const ledger = new SuspiciousTrackLedger(definitions, {filename, now: () => now,
  persistIntervalMs: 0, minimumDurationMs: 2000, minimumMeasuredSamples: 3,
  minimumAbsoluteDopplerHz: 5, minimumDelayMotionKm: .001,
  minimumDopplerMotionHz: 1, maximumSamplesPerTrack: 20});
function frame(id, frequency, index, delayOffset = 0, dopplerScale = 1) {
  const timestamp = 1000 + index * 1000;
  return {id, frequency_hz: frequency, sample_rate_hz: 1_000_000, timestamp,
    capture_epoch: 7, capture_first_input_sample: index * 1000,
    capture_end_input_sample: (index + 1) * 1000,
    tracks: {timestamp, data: [{id: `track-${id}`, state: 'ACTIVE', n: index + 1,
      measured: true, delay: 100 + index * .02 + delayOffset,
      doppler: (-40 + index) * dopplerScale}]}};
}
for (let index = 0; index < 6; index += 1) {
  now = 1000 + index * 1000;
  ledger.accept(frame('a', 509_000_000, index));
  ledger.accept(frame('b', 515_000_000, index, .2, 515 / 509));
  ledger.accept(frame('c', 527_000_000, index, 30, .8));
}
let snapshot = ledger.snapshot({includeSamples: true});
assert.equal(snapshot.decision_effect, 'observation-only');
assert.equal(snapshot.selection_policy.includes('truth-blind'), true);
assert.equal(snapshot.retained_tracks.length, 3);
assert.ok(snapshot.events.some(event => event.channels.length === 3 &&
  event.distinct_transmitter_sites.length === 2));
const event = snapshot.events.find(item => item.channels.length === 3);
assert.equal(event.truth_used_for_selection, false);
assert.ok(event.same_site_consistency.some(value => value.consistent));
assert.equal(snapshot.retained_tracks[0].samples[0].capture_epoch, 7,
  'capture lineage must survive retention');
assert.equal(ledger.attachTruthAssessment(event.event_id,
  {coverage_complete: false, outcome: 'abstain'}), true);
ledger.close();
snapshot = JSON.parse(fs.readFileSync(filename, 'utf8'));
assert.equal(snapshot.assessments[0].role, 'post-inference-evaluation-only');
assert.equal(snapshot.assessments[0].assessment.outcome, 'abstain');

const bounded = new SuspiciousTrackLedger([definitions[0]], {maximumSamplesPerTrack: 3,
  minimumDurationMs: 0, minimumMeasuredSamples: 1, minimumAbsoluteDopplerHz: 0,
  minimumDelayMotionKm: 0, minimumDopplerMotionHz: 0});
for (let index = 0; index < 8; index += 1) bounded.accept(frame('a', 509_000_000, index));
const boundedSnapshot = bounded.snapshot({includeSamples: true});
assert.equal(boundedSnapshot.retained_tracks[0].samples.length, 3);
assert.equal(boundedSnapshot.counters.truncated_track_samples, 5,
  'sample overflow must be explicit rather than silent');

const totalBounded = new SuspiciousTrackLedger([definitions[0]], {maximumSamplesPerTrack: 8,
  maximumTotalSamples: 4, minimumDurationMs: 0, minimumMeasuredSamples: 1,
  minimumAbsoluteDopplerHz: 0, minimumDelayMotionKm: 0, minimumDopplerMotionHz: 0});
for (let index = 0; index < 8; index += 1) totalBounded.accept(frame('a', 509_000_000, index));
assert.ok(totalBounded.snapshot().counters.evicted_active_tracks > 0,
  'the total sample bound must apply even when every sample is still active');

console.log('Suspicious-track bounded truth-blind retention tests passed.');
