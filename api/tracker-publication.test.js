'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {TrackerPublication, JsonlTailer, publicationSettings} =
  require('./tracker-publication');

function config(mode = 'hybrid') {
  return {process: {tracker: {publication: {mode,
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
      {id: 'same', state: 'ACTIVE', delay: 10.5, doppler: 22, n: 20},
      {id: 'weak', state: 'ASSOCIATED', delay: 30, doppler: -50, n: 4},
      {id: 'extra', state: 'COASTING', delay: 40, doppler: -80, n: 7},
      {id: 'invalid', state: 'ACTIVE', delay: null, doppler: 0, n: 99}
    ]});
}

function publishesHybridWithoutDuplicate() {
  let now = 10_000;
  const publication = new TrackerPublication(config(), {now: () => now});
  publication.acceptNative(native());
  publication.acceptInput('{"timestamp":1000}', true);
  assert.equal(publication.acceptExternal(event('u1', 1000, 10, 20)), true);
  const frame = JSON.parse(publication.frame());
  assert.equal(frame.publication.mode, 'hybrid');
  assert.equal(frame.publication.unassisted, 1);
  assert.equal(frame.publication.nativeSupplement, 1);
  assert.deepEqual(frame.data.map(item => item.id), ['u1', 'native-extra']);
  assert.equal(frame.data[0].source, 'unassisted');
  assert.equal(frame.data[1].confidence, 'supplement');
  assert.equal(publication.status().fusion.nativeDeduplicated, 1);
  now += 2001;
  assert.equal(publication.frame(), native());
  assert.equal(publication.status().fusion.fallbackActive, true);
}

function preservesNativeCompatibility() {
  const publication = new TrackerPublication({process: {tracker: {}}});
  publication.acceptNative(native());
  publication.acceptExternal(event('ignored', 1000, 1, 1));
  assert.equal(publication.frame(), native());
  assert.equal(publication.nativeOnly(), native());
  assert.equal(publicationSettings({process: {tracker: {}}}).mode, 'native');
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
  supportsExternalOnlyAndRejectsBadEvents();
  parsesBoundedJsonLines();
  await tailsOnlyNewEventsAndFollowsRotation();
  console.log('tracker-publication tests passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
