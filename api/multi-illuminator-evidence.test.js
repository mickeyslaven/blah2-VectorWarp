'use strict';

const assert = require('node:assert/strict');
const {laneConfig, statisticsFilename, MultiIlluminatorEvidence} =
  require('./multi-illuminator-evidence');

const config = {capture: {fc: 100000000, fs: 2400000}, location: {
  rx: {latitude: 40, longitude: -80, altitude: 300},
  tx: {latitude: 41, longitude: -81, altitude: 200}},
process: {multi_illuminator: {enabled: true, illuminators: [
  {id: 'fm-a', enabled: true, standard: 'FM', frequency_hz: 90100000,
    output_sample_rate_hz: 300000,
    transmitter: {latitude: 40.1, longitude: -79.9, altitude: 250}},
  {id: 'fm-b', enabled: true, standard: 'FM', frequency_hz: 90500000,
    output_sample_rate_hz: 250000,
    transmitter: {latitude: 40.2, longitude: -79.8, altitude: 260}}]}}};

const first = laneConfig(config, config.process.multi_illuminator.illuminators[0]);
assert.equal(first.capture.fc, 90100000);
assert.equal(first.capture.fs, 300000);
assert.equal(first.location.tx.longitude, -79.9);
assert.equal(config.capture.fs, 2400000, 'lane projection must not mutate the global config');
assert.match(statisticsFilename('/tmp/evidence.json', 'fm/a', 'exact'),
  /evidence-fm_a-exact\.json$/);

const created = [];
function fakeStatistics(lane, timing) {
  return {key: `${lane.capture.fc}-${timing}`, configEpoch: `config-${lane.capture.fc}`,
    receiverEpoch: `receiver-${lane.capture.fs}`, accepted: [],
    acceptDetection(value) { this.accepted.push(['detection', value]); },
    acceptTracker(value) { this.accepted.push(['tracker', value]); },
    acceptTiming(value) { this.accepted.push(['timing', value]); },
    liveStatus() { return {schema: 'score', accepted: this.accepted.length}; },
    radarCoverage() { return {schema: 'coverage', points: []}; },
    status() { return {schema: 'statistics'}; }, close() { this.closed = true; }};
}
const evidence = new MultiIlluminatorEvidence(config, {get() {}}, {
  createLanes(lane) {
    const value = {exact: fakeStatistics(lane, 'exact'),
      operational: fakeStatistics(lane, 'provisional')};
    created.push(value); return value;
  }});
evidence.accept({id: 'fm-a', timestamp: 1000, first_input_sample: 0,
  end_input_sample: 149999, detections: {timestamp: 1000}, tracks: {timestamp: 1000}});
assert.equal(created[0].exact.accepted.length, 3);
assert.equal(created[1].exact.accepted.length, 0, 'products cannot leak between lanes');
const lane = evidence.response('scorecard', {illuminator: 'fm-a'});
assert.equal(lane.scope, 'single-illuminator');
assert.equal(lane.identity.sample_rate_hz, 300000);
assert.equal(lane.identity.transmitter.longitude, -79.9);
const aggregate = evidence.response('coverage');
assert.equal(aggregate.scope, 'all-illuminators-non-merged');
assert.equal(aggregate.illuminators.length, 2);
assert.equal(aggregate.points, undefined, 'aggregate must not merge unlike lane coverage');
assert.equal(evidence.response('scorecard', {illuminator: 'missing'}), null);
evidence.close();
assert.ok(created.every(value => value.exact.closed && value.operational.closed));

console.log('Per-illuminator evidence identities, isolation and non-merged aggregate passed.');
