'use strict';
const assert = require('node:assert/strict');
const {geographicContext, geographicConfig, forGeographicLane, validRecordedSites, setReplaySiteStatusProvider} = require('./replay-geographic-sites');
const {locationsFromBearing} = require('./bistatic-geolocation');
const {buildObservationBundle} = require('./radar-observation-bundle');
const {DelayDopplerHistory} = require('./adsb-geometry');
const {fuseTrackCohort, MultiIlluminatorTrackFuser} = require('./multi-illuminator-track-fusion');
const {cooperativeSources} = require('./node-federation');
const {status} = require('./processor-status');
const clone = value => JSON.parse(JSON.stringify(value));
const rx = {latitude: 40.1, longitude: -80.2, altitude: 300, site_id: 'recorded-rx'};
const tx = {latitude: 40.5, longitude: -80, altitude: 450, site_id: 'recorded-tx'};
const sites = {schema: 'vectorwarp-recorded-geographic-sites-v1', provenance: 'operator-configured-at-acquisition',
  rx, tx, illuminators: {a: {frequencyHz: 100e6, transmitter: tx},
    b: {frequencyHz: 101e6, transmitter: {...tx, longitude: -79.5, site_id: 'tower-b'}}}};
const config = {capture: {fs: 2e6, fc: 100e6, replay: {state: true, file: '/recording.blah2iq',
  recording_sha256: 'a'.repeat(64), sample_rate_hz: 2e6, frequency_hz: 100e6}},
  location: {rx: {...rx, latitude: 10}, tx: {...tx, latitude: 11}},
  process: {bearing: {enable: true}, data: {cpi: .5}, tracker: {initiate: {M: 3, N: 6}},
    multi_illuminator: {enabled: false, illuminators: [
      {id: 'a', enabled: true, frequency_hz: 100e6, output_sample_rate_hz: 2e6, transmitter: {latitude: 12}},
      {id: 'b', enabled: true, frequency_hz: 101e6, output_sample_rate_hz: 2e6, transmitter: {latitude: 13}}]}}};
const proof = {acquisitionIntegrityValid: true, acquisitionHashTrusted: true, exactAcquisition: false,
  geographicSitesAvailable: true, geographicSites: sites, geographicSitesReason: 'authenticated configured sites',
  recordingSha256: 'a'.repeat(64), acquisitionSha256: 'b'.repeat(64)};
const processor = {input: 'replay', receiver: 'Kraken', state: 'playing', error: '',
  file: config.capture.replay.file, sampleRate: 2e6, frequency: 100e6, positionSamples: 0,
  totalSamples: 100, loops: 0, trailingSamples: 0, recording: false, recordingFile: '',
  recordingError: '', recordedSamples: 0, replayProvenance: proof};
let current = null;
setReplaySiteStatusProvider(() => current);
const frame = {timestamp: 1000, sample_rate_hz: 2e6, frequency_hz: 100e6,
  data: [{id: '1', delay: 80, doppler: 20, state: 'ACTIVE', bearing_deg_true: 90, uncertainty_deg: 12}]};
const bundle = () => buildObservationBundle({nodeId: 'n', serverSessionId: 's', configRevision: 'c', config, primaryTracks: frame});
assert.equal(validRecordedSites(sites), true);
assert.equal(status({...processor, replayProvenance: {present: false}}).valid, true,
  'Legacy processor status remains valid but provides no geographic capability');
assert.equal(geographicContext(config).available, false);
assert.equal(locationsFromBearing(frame, config).available, false);
assert.equal(bundle().channels.length, 0);
assert.equal(fuseTrackCohort([], config).available, false);
assert.equal(new MultiIlluminatorTrackFuser(config).update([]).available, false);
assert.deepEqual(cooperativeSources({id: 'n'}, config), []);
const truth = new DelayDopplerHistory(config);
assert.equal(truth.update({now: 1, aircraft: []}).aligned_unavailable, true);
for (const receiver of ['Kraken', 'Usrp', 'HackRF', 'RspDuo']) {
  const checked = status({...processor, receiver}); assert.equal(checked.valid, true);
  current = checked.value;
  assert.deepEqual(geographicConfig(config).location, {rx, tx}, 'Recorded sites override mismatching live sites');
  assert.equal(geographicContext(config).available, true, 'Geographic trust is independent of applied gain readback');
  assert.equal(locationsFromBearing(frame, config).available, true);
  assert.deepEqual(bundle().channels[0].receiver, rx);
  assert.deepEqual(bundle().channels[0].transmitter, tx);
  truth.update({now: 2, aircraft: []}); assert.deepEqual(truth.rxSite, rx);
}
for (const mutate of [p => {p.replayProvenance = undefined;},
  p => {p.replayProvenance.acquisitionHashTrusted = false;},
  p => {p.replayProvenance.recordingSha256 = 'c'.repeat(64);},
  p => {p.replayProvenance.geographicSites = null;}, p => {p.file = '/other.blah2iq';}]) {
  current = clone(processor); mutate(current);
  assert.equal(geographicContext(config).available, false);
  assert.equal(bundle().channels.length, 0);
  assert.equal(truth.update({now: 3, aircraft: []}).aligned_unavailable, true);
}
current = clone(processor);
const multi = clone(config); multi.process.multi_illuminator.enabled = true;
assert.deepEqual(geographicConfig(multi).process.multi_illuminator.illuminators[1].transmitter,
  sites.illuminators.b.transmitter);
assert.deepEqual(geographicConfig(forGeographicLane(multi, 'b')).location.tx,
  sites.illuminators.b.transmitter, 'Per-lane truth uses the recorded lane transmitter');
const missing = clone(processor); delete missing.replayProvenance.geographicSites.illuminators.b;
current = missing; assert.equal(geographicContext(multi).available, false);
current = clone(processor); current.replayProvenance.geographicSites.illuminators.b.frequencyHz++;
assert.equal(geographicContext(multi).available, false);
current = clone(processor);
const adjacent = clone(config); adjacent.capture.replay.acquisition_sha256 = 'c'.repeat(64);
assert.equal(geographicContext(adjacent).available, false);
adjacent.capture.replay.acquisition_sha256 = 'b'.repeat(64);
assert.equal(geographicContext(adjacent).available, true);
assert.equal(geographicContext(multi).available, true, 'Trust recovery is reversible');
for (const change of [p => {p.replayProvenance.geographicSites.rx.latitude = 91;},
  p => {delete p.replayProvenance.geographicSites.rx.altitude;},
  p => {p.replayProvenance.acquisitionHashTrusted = false;}]) {
  const invalid = clone(processor); change(invalid); assert.equal(status(invalid).valid, false);
}
current = null;
const live = clone(config); live.capture.replay.state = false;
assert.deepEqual(geographicConfig(live), live, 'Live configuration semantics are unchanged');
console.log('Replay geographic sites: trusted/missing/tamper/mismatch, four receivers, multi-lane identity and recovery passed.');
