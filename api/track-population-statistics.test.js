'use strict';
const assert = require('node:assert/strict');
const {TrackPopulationStatistics, features} = require('./track-population-statistics');
{
  const observed = new TrackPopulationStatistics();
  for (let timestamp = 1000; timestamp <= 601000; timestamp += 500)
    observed.accept({timestamp, data: timestamp === 1000 ?
      [{id: 'unmatched', state: 'ACTIVE', delay: 1, doppler: 2}] : []});
  const window = observed.operationalWindow(601000);
  assert.equal(window.complete, true);
  assert.equal(window.unmatched_tracks, 1);
  assert.equal(new TrackPopulationStatistics(observed.state).operationalWindow(601000).complete,
    false, 'persisted population cannot manufacture post-restart coverage');
  observed.evictedThrough = 1000;
  assert.equal(observed.operationalWindow(601000).complete, false, 'eviction inside window is incomplete');
  observed.evictedThrough = null;
  assert.equal(observed.operationalWindow(605000).complete, false, 'stale population is incomplete');
  observed.accept({timestamp: 605000, data: []});
  assert.equal(observed.operationalWindow(605000).complete, false, 'a gap restarts coverage');
  observed.accept({timestamp: 600000, data: []});
  assert.equal(observed.observedSince, 600000, 'clock regression restarts coverage');
}
const population = new TrackPopulationStatistics();
const frame = (timestamp, state, delay, measured) => population.accept({timestamp,
  data: [{id: 'native', state, delay, doppler: -200,
    ...(measured === undefined ? {} : {measured})}]});
frame(1000, 'ACTIVE', 1);
frame(1500, 'COASTING', 2);
frame(2000, 'COASTING', 3);
frame(2500, 'ACTIVE', 1.2);
frame(3000, 'ACTIVE', 4, false); // explicitly predicted guided publication
frame(3500, 'CONFIRMED', 1.3);
frame(4000, 'TENTATIVE', 100); // not operational: never retained as measured
const track = population.state.active.native;
assert.equal(track.observations, 6);
assert.equal(track.measured_observations, 3);
assert.deepEqual(track.samples.map(row => row.measured), [true, false, false, true, false, true]);
assert.ok(Math.abs(track.delay_motion_km - .3) < 1e-12);
assert.equal(features(track).measured_fraction, .5);
assert.equal(features(track).mean_signed_doppler_hz, -200);
assert.equal(track.state_counts.COASTING, 2);
// Explicit producer provenance wins, including for a legacy nonstandard
// state/measurement combination; missing/malformed flags cannot make coasts hit.
frame(4500, 'COASTING', 1.4, true);
frame(5000, 'COASTING', 20, 'true');
assert.equal(track.measured_observations, 4);
assert.equal(track.samples.at(-1).measured, false);
const prior = track.observations;
frame(5000, 'COASTING', 20);
assert.equal(track.observations, prior, 'duplicate CPI cannot count twice');
const old = {schema: 'vectorwarp-track-population-v2', active: {}, finalized: [{
  key: 'legacy', id: 'legacy', observations: 5, measured_observations: 5,
  state_counts: {ACTIVE: 2, COASTING: 3}, samples: []}], reference: []};
const retained = new TrackPopulationStatistics(old);
assert.equal(retained.state.finalized[0].measured_observations, 5,
  'do not fabricate a per-CPI historical migration without retained states');
console.log('PASS track population legacy coasts are predictions; explicit provenance and history preserved');
