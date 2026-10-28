'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {RecordingLibrary, encodeId, decodeId} = require('./recording-library');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-recordings-'));
const now = Date.UTC(2026, 9, 28, 12);
const config = {save: {path: directory, retention: {automatic_cleanup: true,
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
const listed = library.list({passes: [{pass_id: 'pass-one', hex: 'a1b2c3', flight: 'TEST123'}]});
assert.equal(listed.total, 2);
assert.equal(listed.recordings[0].kind, 'manual');
assert.equal(listed.recordings[1].aircraft[0].flight, 'TEST123');
assert.match(listed.recordings[1].title, /TEST123/);
assert.equal(listed.recordings[1].duration_seconds, 4);
assert.equal(library.list({search: 'a1b2c3'}).total, 1);
assert.equal(decodeId(encodeId('aircraft.mchq')), 'aircraft.mchq');
assert.equal(decodeId(encodeId('../escape.mchq')), null);
assert.equal(library.resolve(encodeId('manual.blah2iq')), manual);

let cleaned = library.cleanup({activeFile: automatic});
assert.equal(cleaned.deleted.length, 0, 'active recordings are never deleted');
cleaned = library.cleanup();
assert.deepEqual(cleaned.deleted.map(item => item.filename), ['aircraft.mchq']);
assert.equal(fs.existsSync(automatic), false);
assert.equal(fs.existsSync(`${automatic}.json`), false);
assert.equal(fs.existsSync(manual), true);
fs.rmSync(directory, {recursive: true, force: true});
console.log('Recording library metadata, search, safe paths and retention passed.');
