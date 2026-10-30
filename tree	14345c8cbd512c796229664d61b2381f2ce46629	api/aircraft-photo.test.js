'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {createAircraftPhotoLookup, normalizePlanespotters} = require('./aircraft-photo');

assert.deepEqual(normalizePlanespotters({photos: []}),
  {available: false, provider: 'Planespotters.net'});
assert.deepEqual(normalizePlanespotters({photos: [{thumbnail: {
  src: 'https://t.plnspttrs.net/123.jpg'}, link:
  'https://www.planespotters.net/photo/123', photographer: 'A Photographer'}]}), {
  available: true, provider: 'Planespotters.net',
  thumbnail: 'https://t.plnspttrs.net/123.jpg',
  link: 'https://www.planespotters.net/photo/123', photographer: 'A Photographer'
});
assert.equal(normalizePlanespotters({photos: [{thumbnail: {src: 'https://evil.invalid/a.jpg'},
  link: 'https://www.planespotters.net/photo/123'}]}).available, false,
  'Untrusted image hosts must not reach the browser');

(async () => {
  const disabled = createAircraftPhotoLookup({truth: {adsb: {
    aircraft_photo_provider: 'disabled'}}}, {fetchJson: async () => {
    throw new Error('Disabled lookup must not use the network');
  }});
  assert.deepEqual(await disabled.lookup('A0B1C2'),
    {available: false, enabled: false, provider: null});

  let calls = 0;
  const enabled = createAircraftPhotoLookup({truth: {adsb: {
    aircraft_photo_provider: 'planespotters'}}}, {fetchJson: async url => {
    calls++;
    assert.equal(url.href, 'https://api.planespotters.net/pub/photos/hex/a0b1c2');
    return {photos: [{thumbnail: {src: 'https://t.plnspttrs.net/a.jpg'},
      link: 'https://www.planespotters.net/photo/a', photographer: 'Tester'}]};
  }});
  const first = await enabled.lookup('A0B1C2');
  const second = await enabled.lookup('a0b1c2');
  assert.equal(first.available, true);
  assert.deepEqual(second, first);
  assert.equal(calls, 1, 'Repeated selected-aircraft requests must use the bounded cache');
  const cacheDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-photo-cache-'));
  const cacheFile = path.join(cacheDirectory, 'photos.json');
  const durable = createAircraftPhotoLookup({truth: {adsb: {
    aircraft_photo_provider: 'planespotters'}}}, {cacheFile, fetchJson: async () => ({photos: [{
      thumbnail: {src: 'https://t.plnspttrs.net/durable.jpg'},
      link: 'https://www.planespotters.net/photo/durable'}]})});
  await durable.lookup('123abc');
  await new Promise(resolve => setTimeout(resolve, 75));
  const restored = createAircraftPhotoLookup({truth: {adsb: {
    aircraft_photo_provider: 'planespotters'}}}, {cacheFile, fetchJson: async () => {
    throw new Error('durable cache should avoid a second provider request');
  }});
  assert.equal((await restored.lookup('123abc')).available, true,
    'Photo metadata survives an API restart');
  fs.rmSync(cacheDirectory, {recursive: true, force: true});
  let failures = 0;
  const failing = createAircraftPhotoLookup({truth: {adsb: {
    aircraft_photo_provider: 'planespotters'}}}, {fetchJson: async () => {
    failures++; throw new Error('provider unavailable');
  }});
  assert.equal((await failing.lookup('abcdef')).temporarily_unavailable, true);
  assert.equal((await failing.lookup('ABCDEF')).temporarily_unavailable, true);
  assert.equal(failures, 1, 'Provider errors must be negatively cached instead of retried per CPI');
  await assert.rejects(() => enabled.lookup('../../etc/passwd'), /six hexadecimal/i);
  console.log('Aircraft photo provider: opt-in, sanitized and cached lookup passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
