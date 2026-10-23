'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('jsdom');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('../../api/config-manager');
const {setupDefaults} = require('../../api/config-store');

const clone = value => JSON.parse(JSON.stringify(value));
const saved = setupDefaults();
saved.location.rx.latitude = 40.4420304;
saved.location.rx.longitude = -80.0566972;
saved.truth.adsb.tar1090 = 'http://pi-zero.local/tar1090';
saved.truth.adsb.sources = [
  'http://pi-zero.local/tar1090',
  'http://strix.local:8080',
  'exchange:https://opendata.adsb.fi/api/v3/lat/40.4420304/lon/-80.0566972/dist/60'
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
    assert.equal(field.querySelector('label').textContent, 'ADS-B sources');
    const locals = field.querySelectorAll('[data-adsb-source="local"]');
    assert.deepEqual([...locals].map(input => input.value),
      ['http://pi-zero.local/tar1090', 'http://strix.local:8080']);
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
    assert.equal(await window.validateActiveConfiguration(), true);
    assert.deepEqual(candidate.truth.adsb.sources, [
      'http://pi-zero.local/tar1090',
      'http://strix.local:8080',
      'http://backup.local/tar1090',
      'exchange:https://opendata.adsb.fi/api/v3/lat/40.4420304/lon/-80.0566972/dist/60',
      'exchange:https://api.adsb.lol/v2/point/40.4420304/-80.0566972/60'
    ]);
    assert.equal(candidate.truth.adsb.tar1090, 'http://pi-zero.local/tar1090');
    console.log('ADS-B source settings DOM: multiple local inputs, exchange switches and newest-source wording passed.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
