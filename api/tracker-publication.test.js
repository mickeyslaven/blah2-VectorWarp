'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {TrackerPublication, JsonlTailer, publicationSettings} =
  require('./tracker-publication');

function config(mode = 'hybrid', sampleRate = 2_400_000) {
  return {capture: {fs: sampleRate}, process: {tracker: {publication: {mode,
    event_file: '/run/vectorwarp/unassisted.jsonl', stale_after_s: 2,
    native_min_associations: 5, delay_tolerance: 2,
    doppler_tolerance: 5}}}};
}

function event(id, timestamp, delay, doppler, length = 8) {
  return {track_id: id, timestamp, length,
    detections: [{timestamp: timestamp - 500, delay: delay - .1, doppler: doppler - 1},
      {timestamp, delay, doppler}], is_anomalous: false, max_velocity_ms: 20,
    anomaly_types: []};
}

function native(timestamp = 1000) {
  return JSON.stringify({timestamp, n: 4, nTentative: 1, nAssociated: 1,
    nActive: 1, nCoasting: 1, data: [
      {id: 'same', state: 'ACTIVE', delay: 315.92, doppler: 22, n: 20},
      {id: 'weak', state: 'ASSOCIATED', delay: 30, doppler: -50, n: 4},
      {id: 'extra', state: 'COASTING', delay: 40, doppler: 22, n: 7},
      {id: 'invalid', state: 'ACTIVE', delay: null, doppler: 0, n: 99}
    ]});
}

function publishesHybridWithoutDuplicate() {
  let now = 10_000;
  const publication = new TrackerPublication(config(), {now: () => now});
  publication.acceptNative(native());
  publication.acceptInput('{"timestamp":1000}', true);
  assert.equal(publication.acceptExternal(event('u1', 1000, 39.46268, 20)), true);
  const frame = JSON.parse(publication.frame());
  assert.equal(frame.publication.mode, 'hybrid');
  assert.equal(frame.publication.unassisted, 1);
  assert.equal(frame.publication.nativeSupplement, 1);
  assert.deepEqual(frame.data.map(item => item.id), ['u1', 'native-extra']);
  assert.equal(frame.data[0].source, 'unassisted');
  assert.equal(frame.data[1].confidence, 'supplement');
  assert.deepEqual(frame.data[1], {...JSON.parse(native()).data[2],
    id: 'native-extra', source: 'native', confidence: 'supplement'});
  assert.equal(publication.status().fusion.nativeDeduplicated, 1);
  now += 2001;
  assert.equal(publication.frame(), native());
  assert.equal(publication.status().fusion.fallbackActive, true);
}

function preservesNativeCompatibility() {
  const raw = `  ${native()}\n`;
  for (const mode of ['native', 'hybrid', 'external']) {
    let now = 10_000;
    const publication = new TrackerPublication(config(mode), {now: () => now});
    publication.acceptNative(raw);
    publication.acceptInput({timestamp: 1000});
    assert.equal(publication.acceptExternal(event('ignored', 1000, 1, 1)), true);
    assert.strictEqual(publication.nativeOnly(), raw);
    if (mode === 'native') assert.strictEqual(publication.frame(), raw);
    now += 2001;
    assert.strictEqual(publication.frame(), raw, `${mode}: stale input preserves native bytes`);
    assert.strictEqual(publication.nativeOnly(), raw);
  }
  assert.equal(publicationSettings({process: {tracker: {}}}).mode, 'native');
}

function convertsExternalDelayAndHistory() {
  for (const [sampleRate, expected] of [
    [2_400_000, [6.324375244956963, 7.1249290734325275]],
    [2_000_000, [5.270312704130802, 5.937440894527106]]
  ]) {
    const publication = new TrackerPublication(config('external', sampleRate), {now: () => 10});
    const input = Object.freeze({timestamp: 1000, delay: Object.freeze([.89]),
      doppler: Object.freeze([20])});
    const external = event('kilometres', 1000, .89, 20);
    external.detections.forEach(Object.freeze);
    Object.freeze(external.detections);
    Object.freeze(external);
    const original = JSON.stringify(external);
    publication.acceptInput(input);
    assert.equal(publication.acceptExternal(external), true);
    const stored = publication.external.get('kilometres');
    const published = JSON.parse(publication.frame()).data[0];
    for (const track of [stored, published]) {
      assert.ok(Math.abs(track.delay - expected[1]) < 1e-12);
      assert.equal(track.associated_delay.length, expected.length);
      track.associated_delay.forEach((delay, index) => {
        assert.ok(Math.abs(delay - expected[index]) < 1e-12);
        const kilometres = delay * 299792458 / sampleRate / 1000;
        assert.ok(Math.abs(kilometres - external.detections[index].delay) < 1e-12);
      });
      assert.deepEqual(track.associated_doppler, [19, 20]);
    }
    assert.equal(JSON.stringify(external), original);
    assert.deepEqual(input.delay, [.89]);
  }
}

function rejectsInvalidSampleRatesWithFullNativeFallback() {
  const raw = `  ${native()}\n`;
  const captureSettings = [{}, null, ...[undefined, null, 0, -1, NaN, Infinity,
    -Infinity, '2400000', Number.MAX_VALUE].map(fs => ({fs}))];
  for (const mode of ['native', 'hybrid', 'external']) {
    for (const capture of captureSettings) {
      const settings = config(mode);
      settings.capture = capture;
      const publication = new TrackerPublication(settings, {now: () => 10});
      publication.acceptNative(raw);
      publication.acceptInput({timestamp: 1000});
      assert.equal(publication.acceptExternal(event('invalid-rate', 1000, .89, 20)), false);
      assert.strictEqual(publication.frame(), raw);
      assert.strictEqual(publication.nativeOnly(), raw);
      const status = publication.status();
      assert.equal(status.externalTracks, 0);
      assert.equal(status.counters.invalid, 1);
      assert.equal(status.externalFresh, false);
      if (mode !== 'native') assert.equal(status.fusion.fallbackActive, true);
    }
  }
}

function supportsExternalOnlyAndRejectsBadEvents() {
  const publication = new TrackerPublication(config('external'), {now: () => 10});
  publication.acceptNative(native());
  publication.acceptInput({timestamp: 2}, true);
  assert.equal(publication.acceptExternal({timestamp: 1, track_id: 'bad', detections: []}), false);
  assert.equal(publication.acceptExternal(event('good', 2, 3, 4)), true);
  const frame = JSON.parse(publication.frame());
  assert.deepEqual(frame.data.map(item => item.id), ['good']);
  assert.equal(frame.publication.nativeSupplement, 0);
  assert.equal(publication.status().counters.invalid, 1);
}

function parsesBoundedJsonLines() {
  let callback;
  const events = [];
  const tailer = new JsonlTailer('/unused', value => { events.push(value); return true; },
    {schedule: fn => { callback = fn; return {unref() {}}; }, cancel() {}});
  assert.equal(typeof callback, 'function');
  tailer.consume('{"track_id":"a"}\n{"track_id"');
  tailer.consume(':"b"}\nnot-json\n');
  assert.deepEqual(events.map(item => item.track_id), ['a', 'b']);
  assert.equal(tailer.status().lines, 2);
  assert.equal(tailer.status().invalid, 1);
  tailer.close();
}

async function tailsOnlyNewEventsAndFollowsRotation() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-tracker-'));
  const filename = path.join(directory, 'events.jsonl');
  fs.writeFileSync(filename, '{"track_id":"history"}\n');
  const events = [];
  const tailer = new JsonlTailer(filename, value => { events.push(value); return true; },
    {schedule: () => ({unref() {}}), cancel() {}});
  await tailer.poll();
  assert.deepEqual(events, [], 'history present before startup must be ignored');
  fs.appendFileSync(filename, '{"track_id":"live"}\n');
  await tailer.poll();
  fs.renameSync(filename, `${filename}.1`);
  fs.writeFileSync(filename, '{"track_id":"rotated"}\n');
  await tailer.poll();
  assert.deepEqual(events.map(item => item.track_id), ['live', 'rotated']);
  assert.equal(tailer.status().resets, 1);
  tailer.close();
  fs.rmSync(directory, {recursive: true});
}

(async () => {
  publishesHybridWithoutDuplicate();
  preservesNativeCompatibility();
  convertsExternalDelayAndHistory();
  rejectsInvalidSampleRatesWithFullNativeFallback();
  supportsExternalOnlyAndRejectsBadEvents();
  parsesBoundedJsonLines();
  await tailsOnlyNewEventsAndFollowsRotation();
  console.log('tracker-publication tests passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
