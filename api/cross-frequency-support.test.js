'use strict';

const assert = require('assert/strict');
const {CrossFrequencySupportTracker, MultiIlluminatorTrackFuser} =
  require('./multi-illuminator-track-fusion');

const C = 299792.458;
const receiver = {latitude: 40, longitude: -80, altitude: 250};
const siteA = {latitude: 40, longitude: -79.8, altitude: 400, site_id: 'site-a'};
const siteB = {latitude: 40.2, longitude: -80, altitude: 450, site_id: 'site-b'};
const lane = (id, frequency_hz, transmitter) => ({id, enabled: true, frequency_hz,
  output_sample_rate_hz: 2.4e6, transmitter});
const colocated = [lane('a-low', 500e6, siteA), lane('a-high', 600e6, siteA)];
const separated = [...colocated, lane('b-low', 510e6, siteB), lane('b-high', 610e6, siteB)];
const configFor = definitions => ({location: {rx: receiver}, process: {multi_illuminator:
  {enabled: true, illuminators: definitions}, data: {cpi: 1},
  tracker: {initiate: {M: 3, N: 5, maxAcc: 10}, delete: 1}}});

function cohort(definitions, cpi, values = {}) {
  return definitions.map(definition => {
    const rows = values[definition.id] || [];
    return {id: definition.id, frequency_hz: definition.frequency_hz,
      sample_rate_hz: definition.output_sample_rate_hz,
      capture_sample_position_available: true, capture_epoch: 9,
      capture_first_input_sample: 1000 + cpi * 1000,
      capture_end_input_sample: 2000 + cpi * 1000,
      timestamp: 100000 + cpi * 1000,
      detections: {timestamp: 100000 + cpi * 1000,
        delay: rows.map(row => row.delay),
        doppler: rows.map(row => -row.rate * definition.frequency_hz / C),
        snr: rows.map(row => row.snr ?? 4),
        provenance: rows.map(row => row.provenance || {source: 'raw-cfar'}),
        false_alarm_probability: 1e-6},
      tracks: {timestamp: 100000 + cpi * 1000, data: []}};
  });
}

const alternating = new CrossFrequencySupportTracker(configFor(colocated),
  {m: 3, n: 5});
let alternatingResult;
for (let cpi = 0; cpi < 3; cpi += 1) {
  const selected = colocated[cpi % 2].id;
  alternatingResult = alternating.update(cohort(colocated, cpi,
    {[selected]: [{delay: 12 + .01 * cpi, rate: .01,
      provenance: {source: 'raw-cfar', cpi}}]}));
}
assert.equal(alternatingResult.tracks.length, 1);
assert.equal(alternatingResult.tracks[0].state, 'SUPPORT_ACTIVE');
assert.equal(alternatingResult.tracks[0].supporting_cpis, 3);
assert.deepEqual(alternatingResult.tracks[0].distinct_lanes, ['a-high', 'a-low']);
assert.equal(alternatingResult.tracks[0].geographic_claim, false);
assert.equal(alternatingResult.operational_track_claim, false);
assert.equal(alternatingResult.measurement_semantics.separate_transmitter_sites_raw_merged, false);
assert.equal(alternatingResult.measurement_semantics.held_out_null_calibrated, false);
assert.match(alternatingResult.measurement_semantics.default_gate_model, /one-delay-bin/);
assert.deepEqual(alternatingResult.tracks[0].contributors[0].provenance.detection,
  {source: 'raw-cfar', cpi: 2});
assert.equal(alternatingResult.tracks[0].contributors[0].source_false_alarm, 1e-6);

const simultaneous = new CrossFrequencySupportTracker(configFor(colocated), {m: 2, n: 3});
const simultaneousFrames = cohort(colocated, 0, {
  'a-low': [{delay: 15, rate: .02}], 'a-high': [{delay: 15, rate: .02}]});
const oneVote = simultaneous.update(simultaneousFrames);
assert.equal(oneVote.tracks.length, 1, 'Coincident colocated lanes form one support hypothesis');
assert.equal(oneVote.tracks[0].supporting_cpis, 1);
assert.equal(oneVote.tracks[0].vote_count_this_cpi, 1);
assert.equal(oneVote.tracks[0].contributors.length, 2, 'Both sources remain provenance, not votes');
const replayed = simultaneous.update(structuredClone(simultaneousFrames));
assert.equal(replayed.tracks[0].supporting_cpis, 1, 'A replayed capture CPI cannot vote twice');

const highResolution = colocated.map(item => ({...item, output_sample_rate_hz: 6.144e6}));
const conservativeGate = new CrossFrequencySupportTracker(configFor(highResolution), {m: 2, n: 3});
const separatedWithinOldGate = conservativeGate.update(cohort(highResolution, 0, {
  'a-low': [{delay: 15, rate: 0}],
  'a-high': [{delay: 15.06, rate: 0}]}));
assert.equal(separatedWithinOldGate.tracks.length, 2,
  'Default delay gating is one 6.144 MS/s bin, not the rejected fixed 0.5 km gate');
const conservativeRate = new CrossFrequencySupportTracker(configFor(colocated), {m: 2, n: 3});
const rateSeparated = conservativeRate.update(cohort(colocated, 0, {
  'a-low': [{delay: 15, rate: 0}],
  'a-high': [{delay: 15, rate: .01}]}));
assert.equal(rateSeparated.tracks.length, 2,
  'Default range-rate gating is one Doppler bin, not the rejected 80 m/s gate');

const clutter = new CrossFrequencySupportTracker(configFor(colocated),
  {m: 2, n: 3});
clutter.update(cohort(colocated, 0, {'a-low': [{delay: 5, rate: 0}]}));
const clutterResult = clutter.update(cohort(colocated, 1, {'a-high': [{delay: 20, rate: 0}]}));
assert.equal(clutterResult.tracks.length, 2, 'Separated raw coordinates do not merge as one target');
assert.ok(clutterResult.tracks.every(track => track.state === 'SUPPORT_CANDIDATE'));
assert.ok(clutterResult.tracks.every(track => track.supporting_cpis === 1));

const siteIsolation = new CrossFrequencySupportTracker(configFor(separated), {m: 2, n: 3});
const isolated = siteIsolation.update(cohort(separated, 0, {
  'a-low': [{delay: 8, rate: .01}], 'b-low': [{delay: 8, rate: .01}]}));
assert.equal(isolated.tracks.length, 2, 'Separate transmitter sites never raw-merge');
assert.equal(new Set(isolated.tracks.map(track => track.transmitter_site)).size, 2);

const expiry = new CrossFrequencySupportTracker(configFor(colocated),
  {m: 2, n: 3, deleteAfter: 1, maximumCoastSeconds: 2});
expiry.update(cohort(colocated, 0, {'a-low': [{delay: 10, rate: 0}]}));
expiry.update(cohort(colocated, 1));
assert.equal(expiry.update(cohort(colocated, 2)).tracks.length, 0,
  'Unconfirmed intermittent evidence expires within the bounded miss policy');
const stopped = new CrossFrequencySupportTracker(configFor(colocated),
  {m: 2, n: 3, maximumCoastSeconds: 2});
stopped.update(cohort(colocated, 0, {'a-low': [{delay: 10, rate: 0}]}));
assert.equal(stopped.snapshot(stopped.lastReceiptAt + 3000).tracks.length, 0,
  'Stopped diagnostic input expires without another CPI');

const invalid = new CrossFrequencySupportTracker(configFor(colocated));
const invalidFrames = cohort(colocated, 0, {'a-low': [{delay: 10, rate: 0}]});
invalidFrames[0].frequency_hz += 1;
const invalidResult = invalid.update(invalidFrames);
assert.equal(invalidResult.available, false);
assert.deepEqual(invalidResult.tracks, []);
assert.match(invalidResult.reason, /provenance/);

// The API-facing fuser publishes the diagnostic support product while leaving
// its operational 3-D hypotheses and tracks empty for a one-site cohort.
const exposed = new MultiIlluminatorTrackFuser(configFor(colocated));
let exposedResult;
for (let cpi = 0; cpi < 3; cpi += 1)
  exposedResult = exposed.update(cohort(colocated, cpi,
    {[colocated[cpi % 2].id]: [{delay: 11 + .01 * cpi, rate: .01}]}));
assert.equal(exposedResult.operational_localization, false);
assert.deepEqual(exposedResult.tracks, []);
assert.equal(exposedResult.cross_frequency_support.tracks[0].state, 'SUPPORT_ACTIVE');
assert.equal(exposedResult.cross_frequency_support.geographic_claim, false);

console.log('Fail-closed same-site cross-frequency detection support tests passed.');
