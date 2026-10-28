'use strict';

const assert = require('assert/strict');
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
  await assert.rejects(() => enabled.lookup('../../etc/passwd'), /six hexadecimal/i);
  console.log('Aircraft photo provider: opt-in, sanitized and cached lookup passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
