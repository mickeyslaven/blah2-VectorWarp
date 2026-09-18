'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('jsdom');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('../../api/config-manager');
const {setupDefaults} = require('../../api/config-store');
const {emptyRecord} = require('../../html/js/kraken_geometry');
const {applyDeviceProfile, receiverFailureMessage, receiverMayHaveChanged} = require('../../html/js/config_ui');
const clone = value => JSON.parse(JSON.stringify(value));
let saved = applyDeviceProfile(setupDefaults(), getDeviceProfiles()[0]);
saved.capture.device.array_geometry = emptyRecord(5);
saved.capture.device.array_geometry.mapping_confirmed = true;
saved.capture.device.array_geometry.geometry_confirmed = true;
const originalMeasurements = clone(saved.capture.device.array_geometry.elements);
let mode = 'partial', revision = 'fixture', lastPutTimeout = 0;
const dom = new JSDOM('<div id="configuration"></div>', {url: 'http://127.0.0.1:3000/display/configuration/', runScripts: 'outside-only'});
const window = dom.window;
window.liveApiUrl = value => value;
window.rememberApiPort = () => {};
window.fetchStatusResource = async (url, options, timeout) => {
  if (options?.method === 'PUT') lastPutTimeout = timeout;
  return window.fetch(url, options);
};
window.fetch = async (url, options = {}) => {
  let body, status = 200;
  if (url === '/api/config/capabilities') body = {editable: true, restartAvailable: false, configRevision: revision, deviceProfiles: getDeviceProfiles(), fieldRules: FIELD_RULES};
  else if (url === '/api/config') body = saved;
  else if (url === '/api/config/validate') body = validateConfig(JSON.parse(options.body), saved);
  else if (url === '/api/system/status') body = {serverId: 'fixture', configRevision: revision, loadedRevision: revision, radar: 'no-data', errors: []};
  else if (url === '/api/upstream/status') body = {supported: false};
  else if (url.startsWith('/api/config?')) {
    assert.equal(options.headers['X-VectorWarp-Receiver-Sync'], 'synchronize-v1');
    if (mode === 'transport') throw new Error('Connection lost');
    if (mode === 'partial') {
      status = 409; body = {errors: ['Frequency command rejected.'], receiverSync: {
        receiverType: 'Kraken', status: 'partial', configPersisted: false,
        operations: [{operation: 'set_num_elements', commandSent: true, commandOutcome: 'acknowledged', acknowledged: true, readbackMatched: true}]
      }};
    } else {
      saved = JSON.parse(options.body); revision = 'saved';
      body = {ok: true, config: saved, revision, restarting: false, message: 'Saved; restart manually.', receiverSync: {receiverType: 'Kraken', status: 'not-required', operations: []}};
    }
  } else throw new Error(`Unexpected fixture URL ${url}`);
  return {ok: status === 200, status, headers: {get: () => `"${revision}"`}, json: async () => clone(body)};
};
for (const file of ['kraken_geometry.js', 'config_ui.js']) window.eval(fs.readFileSync(path.join(__dirname, '../../html/js', file), 'utf8'));
const query = key => window.document.querySelector(`[data-path="${key}"] input, [data-path="${key}"] select`);
const mapping = () => query('capture.device.array_geometry.mapping_confirmed');
const message = () => window.document.getElementById('config-message').textContent;
function change(input, value) { input.value = String(value); input.dispatchEvent(new window.Event('input', {bubbles: true})); }
(async () => {
  try {
    await window.renderConfiguration();
    change(query('process.data.cpi'), .4);
    await window.saveConfiguration();
    assert.ok(lastPutTimeout >= 72000, 'Browser deadline must exceed all configured server stage deadlines');
    assert.ok(message().includes('Receiver settings may already have changed'));
    assert.ok(message().includes('VectorWarp settings were not saved'));
    assert.equal(saved.capture.device.array_geometry.mapping_confirmed, true, 'Failure does not rewrite the saved file');
    assert.equal(mapping().checked, false, 'Partial hardware outcome invalidates the browser draft');
    window.document.getElementById('config-reset').click();
    assert.equal(mapping().checked, false, 'Discard must not restore an assertion invalidated by a known receiver transaction');
    assert.deepEqual(saved.capture.device.array_geometry.elements, originalMeasurements);
    mode = 'transport'; change(query('process.data.cpi'), .3); await window.saveConfiguration();
    assert.ok(message().includes('Save not confirmed'));
    assert.ok(message().includes('receiver settings may still change'));
    assert.equal(window.document.getElementById('config-restart-label').textContent, 'Save not confirmed.');
    assert.equal(mapping().checked, false);
    mode = 'success'; await window.saveConfiguration();
    assert.equal(saved.capture.device.array_geometry.mapping_confirmed, false);
    assert.equal(mapping().checked, false, 'Canonical saved response matches the editor');
    assert.equal(receiverMayHaveChanged({status: 'failed', operations: [{commandSent: true, commandOutcome: 'rejected'}]}), false);
    assert.equal(receiverMayHaveChanged({status: 'indeterminate', operations: []}), true);
    const persistence = receiverFailureMessage({errors: ['Suite confirmed settings, but the config file could not be saved.'], receiverSync: {status: 'receiver-confirmed-config-not-saved', operations: []}});
    assert.ok(persistence.includes('config file could not be saved'));
    assert.ok(!persistence.includes('Receiver settings may already have changed'));
    console.log('Receiver partial/unknown outcomes, geometry assertions, canonical save and browser deadline DOM gates passed.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
