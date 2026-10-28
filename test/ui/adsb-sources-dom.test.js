'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('./jsdom');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('../../api/config-manager');
const {configurationForReceiver} = require('../../api/config-store');

const clone = value => JSON.parse(JSON.stringify(value));
let saved = configurationForReceiver('RspDuo');
let writes = 0;
saved.location.rx.latitude = 40;
saved.location.rx.longitude = -80;
saved.truth.adsb.tar1090 = 'http://adsb-primary.example.invalid/tar1090';
saved.truth.adsb.sources = [
  'http://adsb-primary.example.invalid/tar1090',
  'http://adsb-secondary.example.invalid/tar1090',
  'exchange:https://opendata.adsb.fi/api/v3/lat/40/lon/-80/dist/60'
];
let candidate = null;
const dom = new JSDOM('<div id="configuration"></div>', {
  url: 'http://127.0.0.1:3000/display/configuration/', runScripts: 'outside-only'
});
const {window} = dom;
window.liveApiUrl = value => value;
window.fetchStatusResource = (url, options) => window.fetch(url, options);
window.rememberApiPort = () => {};
window.fetch = async (url, options = {}) => {
  let body;
  if (url === '/api/config') body = saved;
  else if (url.startsWith('/api/config?')) {
    assert.equal(options.method, 'PUT');
    assert.equal(options.headers['If-Match'], '"adsb-test"');
    const next = JSON.parse(options.body);
    assert.equal(validateConfig(next, saved).valid, true, 'Structured sources are writable');
    saved = next;
    writes++;
    body = {ok: true, restarting: false, revision: 'adsb-test', message: 'Saved locally.'};
  }
  else if (url === '/api/config/capabilities') body = {editable: true, restartAvailable: false,
    configRevision: 'adsb-test', fieldRules: FIELD_RULES,
    deviceProfiles: getDeviceProfiles().map(profile => ({...profile, liveAvailable: false}))};
  else if (url === '/api/config/validate') {
    candidate = JSON.parse(options.body);
    body = validateConfig(candidate, saved);
  } else if (url === '/api/upstream/status') body = {supported: false, matched: false};
  else if (url === '/api/system/status') body = {serverId: 'test', configRevision: 'adsb-test',
    loadedRevision: 'adsb-test', radar: 'no-data', errors: []};
  else throw new Error(`Unexpected URL: ${url}`);
  return {ok: true, status: 200, headers: {get: key => key === 'ETag' ? '"adsb-test"' : null},
    json: async () => clone(body)};
};
window.eval(fs.readFileSync(path.join(__dirname, '../../html/js/config_ui.js'), 'utf8'));

(async () => {
  try {
    await window.renderConfiguration();
    const field = window.document.querySelector('[data-path="truth.adsb.tar1090"]');
    assert.equal(FIELD_RULES['truth.adsb.sources'].readOnly, undefined);
    assert.equal(FIELD_RULES['truth.adsb.sources'].structuredControl, 'truth.adsb.tar1090');
    assert.equal(window.document.querySelector('[data-path="truth.adsb.sources"]'), null,
      'The structured source controls must not have a duplicate generic editor');
    assert.equal(field.querySelector('label').textContent, 'ADS-B sources');
    const locals = field.querySelectorAll('[data-adsb-source="local"]');
    assert.deepEqual([...locals].map(input => input.value),
      ['http://adsb-primary.example.invalid/tar1090',
        'http://adsb-secondary.example.invalid/tar1090']);
    assert.equal(field.querySelector('[data-adsb-exchange="adsb-fi"]').checked, true);
    assert.equal(field.querySelector('[data-adsb-exchange="adsb-lol"]').checked, false);
    assert.match(field.textContent, /newest healthy observation wins/i);
    assert.match(field.textContent, /not averaged/i);

    field.querySelector('.adsb-source-add').click();
    const updatedLocals = field.querySelectorAll('[data-adsb-source="local"]');
    const added = updatedLocals[updatedLocals.length - 1];
    added.value = 'http://backup.local/tar1090';
    added.dispatchEvent(new window.Event('input', {bubbles: true}));
    const lol = field.querySelector('[data-adsb-exchange="adsb-lol"]');
    lol.checked = true;
    lol.dispatchEvent(new window.Event('change', {bubbles: true}));
    const valid = await window.validateActiveConfiguration();
    assert.equal(valid, true, JSON.stringify(validateConfig(candidate, saved)));
    assert.deepEqual(candidate.truth.adsb.sources, [
      'http://adsb-primary.example.invalid/tar1090',
      'http://adsb-secondary.example.invalid/tar1090',
      'http://backup.local/tar1090',
      'exchange:https://opendata.adsb.fi/api/v3/lat/40/lon/-80/dist/60',
      'exchange:https://api.adsb.lol/v2/point/40/-80/60'
    ]);
    assert.equal(candidate.truth.adsb.tar1090,
      'http://adsb-primary.example.invalid/tar1090');
    const expectedSources = clone(candidate.truth.adsb.sources);
    await window.saveConfiguration();
    assert.equal(writes, 1, 'Structured local inputs and exchange switches save successfully');
    assert.deepEqual(saved.truth.adsb.sources, expectedSources);
    console.log('ADS-B source settings DOM: multiple local inputs, exchange switches and newest-source wording passed.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
