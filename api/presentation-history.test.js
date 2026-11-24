'use strict';
const assert = require('assert');
const {RadarSessionStore, PresentationHistory, frameTimestamp, configuredPresentationDelay} = require('./presentation-history');

const presentationConfig = (cpi, lag = 2) => ({process: {data: {cpi}},
  truth: {adsb: {association_lag_cpis: lag}}});
assert.equal(configuredPresentationDelay(presentationConfig(.5)), 1000);
assert.equal(configuredPresentationDelay(presentationConfig(.5, 0)), 0);
for (const cpi of [undefined, null, '0.5', NaN, Infinity, 0, -1])
  assert.equal(configuredPresentationDelay(presentationConfig(cpi)), null,
    'An invalid CPI cannot silently become zero presentation delay');
assert.equal(configuredPresentationDelay(presentationConfig(Number.MAX_VALUE)), null);

assert.equal(frameTimestamp({timestamp: 1000}), 1000);
assert.equal(frameTimestamp(JSON.stringify({frameTimestamp: 1500})), 1500);
assert.equal(frameTimestamp({timestamp: [1000, 2000]}), 2000);
assert.equal(frameTimestamp('invalid'), null);

const history = new PresentationHistory({maxAgeMs: 10000, maxBytes: 10000});
history.add(JSON.stringify({timestamp: 1000, value: 'a'}), 1000);
history.add(JSON.stringify({timestamp: 1500, value: 'b'}), 1500);
history.add(JSON.stringify({timestamp: 2000, value: 'c'}), 2000);
assert.equal(JSON.parse(history.at(1750)).value, 'b');
assert.equal(history.at(999), null);
assert.equal(JSON.parse(history.at(9999)).value, 'c');
assert.equal(JSON.parse(history.exact(1500)).value, 'b');
assert.equal(history.exact(1750), null, 'Exact-frame presentation never substitutes an adjacent CPI');
assert.equal(history.timestampAt(1750), 1500,
  'Presentation clock selection does not need to parse the stored frame again');
assert.deepEqual(history.diagnostics(500), {
  samples: 3, latest_timestamp: 2000, interval_median_ms: 500,
  interval_p95_ms: 500, interval_max_ms: 500, expected_interval_ms: 500,
  within_100ms_fraction: 1
});
history.clear();
assert.equal(history.at(9999), null);

console.log('Aligned presentation history selection PASS.');

// Differential transcript: the shared owner must preserve each retained
// renderer's output, exact lookup, timestamp clock and byte/age eviction.
const stashFactories = require('./stash/frame-history');
let now = 1000;
const session = new RadarSessionStore({now: () => now});
for (const [name, factory] of Object.entries({map: 'maxhold', detection: 'detection',
  timing: 'timing', iqdata: 'iqdata'})) {
  const reference = new PresentationHistory();
  const stash = stashFactories[factory]();
  for (let index = 0; index < 25; index++) {
    now += 1000;
    const value = {timestamp: now, delay: [1, 2], doppler: [3], data: [[index, 1]],
      snr: [4, 5], frequency: [100, 200], spectrum: [index, 0], cpi: 500};
    if (name === 'detection') value.doppler = [3, 4];
    const frame = JSON.stringify(value);
    reference.add(frame, now); stash.update_data(frame);
    session.acceptProduct(name, frame);
    assert.equal(session.products[name], frame);
    assert.equal(session.histories[name].bytes, reference.bytes);
    assert.equal(session.histories[name].at(now - 500), reference.at(now - 500));
    assert.equal(session.histories[name].exact(now), reference.exact(now));
    assert.deepEqual(session.stashes[name].get_data(), stash.get_data());
  }
}

// Receipt time and scientific time are distinct. Replay/stale/future scientific
// timestamps remain payload, and exact uint64 identities remain strings.
const receipt = {timestamp: 1, acceleration: {receipt: {epoch: '9007199254740993',
  first: '9007199254740995', end: '18446744073709551615'}}};
const first = session.open('timing');
assert.equal(session.activate(first), true);
session.acceptProduct('timing', JSON.stringify(receipt));
const received = now;
assert.equal(session.fresh('timing', 100, received + 99), true);
assert.equal(session.fresh('timing', 100, received + 100), false);
assert.equal(session.fresh('timing', 100, received + 100, true), true);
assert.deepEqual(JSON.parse(session.products.timing), receipt);
now += 100;
assert.equal(session.acceptProduct('timing', JSON.stringify(receipt)), false);
assert.equal(session.fresh('timing', 100, now), true,
  'Retransmission refreshes receipt liveness without another scientific frame');
const second = session.open('timing'); session.activate(second);
assert.equal(session.close(first), false);
assert.notEqual(session.products.timing, '');
assert.equal(session.close(second), true);
assert.equal(session.products.timing, '');
assert.equal(session.channel('timing').receivedAt, null);
assert.deepEqual(JSON.parse(session.histories.timing.exact(1)), receipt,
  'Closing telemetry retains historical aligned presentation');
const invalidated = session.open('timestamp');
session.invalidate('timestamp');
assert.equal(session.activate(invalidated), false);
const oldGeneration = session.open('timing');
session.restart();
assert.equal(session.activate(oldGeneration), false);
const terminal = JSON.stringify({timestamp: 2, captureTerminal: true, nCpi: 20});
session.acceptProduct('timing', terminal, {history: false});
assert.equal(session.products.timing, terminal);
assert.equal(session.histories.timing.exact(2), null);

const descriptor = {id: 'fm', enabled: true, frequency_hz: 100000000,
  output_sample_rate_hz: 200000};
const config = {process: {multi_illuminator: {enabled: true, illuminators: [descriptor]}}};
const laneFrame = (timestamp, first = 10, extra = {}) => ({
  schema: 'vectorwarp-multi-illuminator-frame-v1', id: 'fm', timestamp,
  frequency_hz: 100000000, sample_rate_hz: 200000,
  first_input_sample: first, end_input_sample: first + 9,
  temporalShadow: {banks: [{map: {data: [[-99]]}}]},
  map: {timestamp, delay: [1], doppler: [2], data: [[3]]},
  detections: {timestamp, delay: [1], doppler: [2], snr: [3]},
  tracks: {timestamp, data: [{id: '9007199254740993'}]}, ...extra});
for (const legacyFirst of [true, false]) {
  const calls = [];
  const store = new RadarSessionStore({config, onProduct: (name, frame) => {
    calls.push({name, frame});
  }});
  const value = laneFrame(100);
  const legacy = () => {
    for (const [name, key] of [['map', 'map'], ['detection', 'detections'], ['tracker', 'tracks']])
      store.acceptProduct(name, JSON.stringify(value[key]));
  };
  if (legacyFirst) legacy();
  const {value: accepted} = store.acceptIlluminator(JSON.stringify(value), {firstFrame: true});
  assert.equal(Object.hasOwn(accepted.map, 'data'), false, 'Matrix stays lazy');
  assert.equal(accepted.map_json, JSON.stringify(value.map));
  assert.equal(accepted.tracks.data[0].id, '9007199254740993');
  store.publishPrimary(accepted);
  if (!legacyFirst) legacy();
  assert.deepEqual(calls.map(call => call.name).sort(), ['detection', 'map', 'tracker'],
    'Legacy and primary aliases invoke each evidence consumer once in either arrival order');
  assert.equal(store.acceptIlluminator(JSON.stringify(value), {firstFrame: true}).duplicate, true,
    'A retransmitted capture frame cannot rerun evidence or recording');
  const {value: compact} = store.acceptIlluminator(JSON.stringify(laneFrame(101, 20,
    {map: {timestamp: 101, data_omitted: true}})), {firstFrame: true});
  store.publishPrimary(compact);
  assert.equal(store.products.map, JSON.stringify(value.map));
  assert.equal(store.latest('fm').presentation_frame.timestamp, 100);
  assert.equal(store.histories.detection.exact(101), JSON.stringify(compact.detections));
  assert.equal(store.histories.map.exact(101), null,
    'Compact metadata cannot relabel a retained display matrix as a new observation');
  const full = store.acceptIlluminator(JSON.stringify(laneFrame(101, 20)));
  assert.equal(full.duplicate, true);
  const before = calls.filter(call => call.name !== 'map').length;
  store.publishPrimary(full.value, full.duplicate);
  assert.equal(calls.filter(call => call.name !== 'map').length, before);
  assert.deepEqual(JSON.parse(store.products.map).data, [[3]],
    'A later full matrix for the same capture interval remains publishable');
  assert.equal(store.latest('fm').presentation_frame.timestamp, 101);
}

let resets = 0;
const restarted = new RadarSessionStore({config, onReset: () => { resets++; }});
restarted.acceptIlluminator(JSON.stringify(laneFrame(100, 100)), {firstFrame: true});
assert.throws(() => restarted.acceptIlluminator(JSON.stringify(laneFrame(101, 0,
  {frequency_hz: 999})), {firstFrame: true}), /active configuration/);
assert.equal(resets, 0);
assert.equal(restarted.latest('fm').timestamp, 100,
  'Rejected configuration cannot destroy the admitted producer session');
restarted.acceptIlluminator(JSON.stringify(laneFrame(101, 0)), {firstFrame: true});
assert.equal(resets, 1);
assert.equal(restarted.exact('fm', {timestamp: 100}), null);
restarted.acceptIlluminator(JSON.stringify(laneFrame(102, 20)), {firstFrame: true});
assert.equal(resets, 1, 'A healthy reconnect retains the producer session');

const bearingStore = new RadarSessionStore({config});
bearingStore.acceptIlluminator(JSON.stringify(laneFrame(103, 30, {
  bearing: {timestamp: 103, illuminator_id: 'fm', resolved_product_plan: '{}',
    input_lineage: {available: true}, product_stage_execution: {bearing: true}}
})), {firstFrame: true});
assert.equal(bearingStore.planObservation().products.filter(product =>
  product.lane_id === 'fm' && product.product === 'bearing').length, 1,
  'Channelized bearing retains its exact lane execution receipt');
const wrongBearingStore = new RadarSessionStore({config});
wrongBearingStore.acceptIlluminator(JSON.stringify(laneFrame(104, 40, {
  bearing: {timestamp: 104, illuminator_id: 'other', resolved_product_plan: '{}'}
})), {firstFrame: true});
assert.equal(wrongBearingStore.planObservation().products.some(product =>
  product.product === 'bearing'), false, 'A lane-less or mismatched bearing receipt is not fanned out');

const envelope = new RadarSessionStore({now: () => now});
const checked = {valid: true, value: {receiver: 'RspDuo', state: 'live'}};
envelope.acceptProcessorStatus(checked, {});
assert.equal(envelope.processorStatus.value, checked.value);
assert.equal(envelope.processorFresh(now + 5000), true);
assert.equal(envelope.processorFresh(now + 5001), false);
envelope.acceptProcessorStatus({valid: false}, {state: 'invalid'});
assert.equal(envelope.processorStatus.value, checked.value);
envelope.acceptProcessorStatus({valid: false}, {receiverHealth: null});
assert.equal(envelope.processorStatus, null);
envelope.acceptProcessorStatus(checked, {});
envelope.acceptProcessorStatus({valid: false}, {replayProvenance: null});
assert.equal(envelope.processorStatus, null);
let projected = null, evidence = 0;
const publication = new RadarSessionStore({projectProduct: (_name, frame) => {
  projected = frame;
  return JSON.stringify({timestamp: JSON.parse(frame).timestamp});
}, onProduct: () => { evidence++; }});
const identities = ['9007199254740992', '9007199254740993'].map(id =>
  JSON.stringify({timestamp: 9, data: [{id}]}));
identities.forEach(frame => publication.acceptProduct('tracker', frame));
assert.equal(evidence, 2, 'Equal timestamps cannot collapse distinct exact identities');
publication.acceptProduct('tracker', identities[0]);
assert.equal(evidence, 2);
assert.equal(projected, identities[0], 'Duplicate delivery still refreshes publication');
assert.equal(publication.histories.tracker.bytes, identities.reduce((bytes, frame) =>
  bytes + Buffer.byteLength(frame) + Buffer.byteLength('{"timestamp":9}'), 0),
  'Distinct retained native/publication payloads both participate in the byte limit');
console.log('Canonical session differential products, aliases, lazy maps, reset and freshness PASS.');

// Framing validates once; the canonical owner shares that object with stashes,
// timestamp retention and evidence adapters without changing original bytes.
const jsonFrames = require('./json-frames');
const originalParse = JSON.parse;
let parses = 0;
JSON.parse = (...args) => { parses++; return originalParse(...args); };
try {
  for (const name of ['map', 'detection', 'tracker', 'bearing', 'timing', 'iqdata']) {
    const value = {timestamp: 123, delay: [1], doppler: [2], data: [[3.125]],
      snr: [4], frequency: [100], spectrum: [5], cpi: 500,
      id: '18446744073709551615'};
    const raw = JSON.stringify(value).replace('3.125', '3.125000');
    let seen;
    const owner = new RadarSessionStore({onProduct: (_name, text, options) => {
      assert.equal(text, raw); seen = options.parsed;
      assert.equal(options.publishedParsed, seen);
    }});
    const before = parses;
    jsonFrames((text, parsed) => owner.acceptProduct(name, text, {parsed}))(raw);
    assert.equal(parses - before, 1, `${name} framing and consumers parse once`);
    assert.deepEqual(seen, value);
    assert.equal(owner.products[name], raw);
    assert.equal(owner.histories[name].exact(123), raw);
    if (name === 'map') assert.deepEqual(owner.stashes.map.get_data().data, [[3.125]]);
  }
  for (const id of ['fm', 'other']) {
    const owner = new RadarSessionStore({config: {process: {multi_illuminator: {
      enabled: true, illuminators: [descriptor, {...descriptor, id: 'other'}]}}}});
    const native = laneFrame(456, 10, {id});
    const raw = JSON.stringify(native).replace('[[3]]', '[[3.000000]]');
    const before = parses;
    jsonFrames((text, parsed) => {
      const admitted = owner.acceptIlluminator(text, {}, parsed);
      assert.equal(admitted.map, parsed.map, 'No second full matrix allocation');
      assert.equal(Object.hasOwn(admitted.value.map, 'data'), false,
        'The lane history retains compact metadata, not the parsed matrix');
      owner.publishPrimary(admitted.value, admitted.duplicate, admitted.map);
    })(raw);
    assert.equal(parses - before, 1, 'Multi-illuminator and primary aliases parse once');
    assert.ok(owner.latest(id).map_json.includes('[[3.000000]]'), 'Raw map precision/spelling retained');
    if (id === 'fm') {
      assert.deepEqual(owner.stashes.map.get_data().data, [[3]]);
      assert.equal(owner.products.map, owner.latest(id).map_json);
    } else assert.equal(owner.products.map, '', 'Nonprimary matrices are not retained in maxhold');
  }
  const invalid = new RadarSessionStore({config});
  assert.throws(() => invalid.acceptIlluminator(JSON.stringify(laneFrame(1, 1, {map: 'invalid'}))),
    /Invalid multi-illuminator frame/);
} finally { JSON.parse = originalParse; }
console.log('Parse-once native products, primary aliases, lazy lane retention and raw precision PASS.');
