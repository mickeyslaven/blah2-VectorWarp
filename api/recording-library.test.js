'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {RecordingLibrary, encodeId, decodeId} = require('./recording-library');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-recordings-'));
const now = Date.UTC(2026, 9, 28, 12);
const config = {save: {path: directory, retention: {automatic_cleanup: true,
  low_space_policy: 'stop_recording',
  maximum_age_days: 1, maximum_total_gb: 0, minimum_free_gb: 0}}};
const automatic = path.join(directory, 'aircraft.mchq');
fs.writeFileSync(automatic, Buffer.alloc(256));
fs.writeFileSync(`${automatic}.json`, JSON.stringify({schema: 'vectorwarp-kraken-iq-clip-v1',
  createdAt: now - 2 * 86400000, channels: 5, sampleRate: 2000000,
  frequency: 509000000, samplesPerChannel: 8000000, receiver: 'Kraken',
  eventIds: ['eligibility:one']}));
fs.writeFileSync(`${automatic}.admission.json`, JSON.stringify({state: 'admitted', captures: {
  one: {pass_id: 'pass-one', icao: 'a1b2c3', truth_samples: [{slant_range_km: 12.5,
    elevation_deg: 4.2, bearing_deg_true: 90}]}}}));
const old = new Date(now - 2 * 86400000);
for (const filename of [automatic, `${automatic}.json`, `${automatic}.admission.json`])
  fs.utimesSync(filename, old, old);
const manual = path.join(directory, 'manual.blah2iq');
fs.writeFileSync(manual, Buffer.alloc(128));
fs.writeFileSync(`${manual}.json`, JSON.stringify({schema: 'vectorwarp-recording-provenance-v2',
  complete: true, createdAt: now - 1000, channels: 2, sampleRate: 1000000,
  frequency: 100000000, samples: 2000000, receiver: 'Usrp'}));

const library = new RecordingLibrary(() => config, {now: () => now});
async function main() {
const backgroundLibrary = new RecordingLibrary(() => config, {now: () => now});
const indexing = await backgroundLibrary.list({limit: 1, waitForIndex: false});
assert.equal(indexing.indexing, true);
assert.equal(indexing.total, 0, 'cold browser request returns without waiting for the archive scan');
await backgroundLibrary.scanPromise;
const indexed = await backgroundLibrary.list({limit: 1, waitForIndex: false});
assert.equal(indexed.indexing, false);
assert.equal(indexed.total, 2);
const listed = await library.list({passes: [{pass_id: 'pass-one', hex: 'a1b2c3', flight: 'TEST123'}]});
assert.equal(listed.total, 2);
assert.equal(listed.recordings[0].kind, 'manual');
assert.equal(listed.recordings[1].aircraft[0].flight, 'TEST123');
assert.match(listed.recordings[1].title, /TEST123/);
assert.equal(listed.recordings[1].duration_seconds, 4);
assert.equal((await library.list({search: 'a1b2c3',
  passes: [{pass_id: 'pass-one', hex: 'a1b2c3', flight: 'TEST123'}]})).total, 1);
assert.equal(decodeId(encodeId('aircraft.mchq')), 'aircraft.mchq');
assert.equal(decodeId(encodeId('../escape.mchq')), null);
assert.equal(library.resolve(encodeId('manual.blah2iq')), manual);
assert.equal((await library.list({kind: 'automatic'})).total, 1);
assert.equal((await library.list({receiver: 'Usrp'})).total, 1);
assert.equal((await library.list({frequency: 509000000})).total, 1);
assert.equal((await library.list({sort: 'oldest'})).recordings[0].filename, 'aircraft.mchq');
const folder = await library.createDirectory('site-a/session-one');
assert.equal(folder.path, `${path.join(directory, 'site-a', 'session-one')}/`);
assert.ok((await library.directories()).directories.some(item => item.path === folder.path));
assert.equal((await library.remove(encodeId('missing.blah2iq'))).reason, 'not-found');
assert.equal((await library.remove(encodeId('manual.blah2iq'), {activeFile: manual})).reason, 'active');

let cleaned = await library.cleanup({activeFile: automatic});
assert.equal(cleaned.deleted.length, 0, 'active recordings are never deleted');
cleaned = await library.cleanup();
assert.deepEqual(cleaned.deleted.map(item => item.filename), ['aircraft.mchq']);
assert.equal(fs.existsSync(automatic), false);
assert.equal(fs.existsSync(`${automatic}.json`), false);
assert.equal(fs.existsSync(manual), true);
assert.equal((await library.remove(encodeId('manual.blah2iq'))).removed, true);
assert.equal(fs.existsSync(manual), false);
fs.rmSync(directory, {recursive: true, force: true});

const policyDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-recording-policy-'));
const policyConfig = {save: {path: policyDirectory, retention: {automatic_cleanup: false,
  low_space_policy: 'stop_recording', maximum_age_days: 0,
  maximum_total_gb: 0, minimum_free_gb: 10000}}};
const policyClip = path.join(policyDirectory, 'old-aircraft.mchq');
const policyManual = path.join(policyDirectory, 'old-manual.blah2iq');
fs.writeFileSync(policyClip, Buffer.alloc(32));
fs.writeFileSync(`${policyClip}.json`, JSON.stringify({createdAt: now - 86400000,
  complete: true, eventIds: ['aircraft:test']}));
fs.writeFileSync(policyManual, Buffer.alloc(32));
fs.writeFileSync(`${policyManual}.json`, JSON.stringify({createdAt: now - 86400000, complete: true}));
for (const filename of [policyClip, `${policyClip}.json`, policyManual, `${policyManual}.json`])
  fs.utimesSync(filename, old, old);
const policyLibrary = new RecordingLibrary(() => policyConfig, {now: () => now});
assert.equal((await policyLibrary.cleanup()).deleted.length, 0,
  'stop-recording policy never deletes clips');
policyConfig.save.retention.low_space_policy = 'delete_oldest';
const policyCleanup = await policyLibrary.cleanup();
assert.deepEqual(policyCleanup.deleted.map(item => item.filename), ['old-aircraft.mchq']);
assert.equal(fs.existsSync(policyManual), true, 'low-space cleanup preserves manual recordings');
fs.rmSync(policyDirectory, {recursive: true, force: true});
console.log('Recording library metadata, search, safe paths and retention passed.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
