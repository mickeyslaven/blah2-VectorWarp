'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('./jsdom');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('../../api/config-manager');
const {setupDefaults} = require('../../api/config-store');
const {emptyRecord, context} = require('../../html/js/kraken_geometry');
const {applyDeviceProfile, receiverFailureMessage, receiverMayHaveChanged} = require('../../html/js/config_ui');
const clone = value => JSON.parse(JSON.stringify(value));
let saved = applyDeviceProfile(setupDefaults(), getDeviceProfiles()[0]);
saved.capture.device.array_geometry = emptyRecord(5);
saved.capture.device.array_geometry.mapping_confirmed = true;
saved.capture.device.array_geometry.geometry_confirmed = true;
const originalMeasurements = clone(saved.capture.device.array_geometry.elements);
// Gain selection cannot revoke physical geometry assertions under either policy.
for (const policy of ['fixed', 'measured', 'measured_per_channel']) {
  const candidate = clone(saved);
  candidate.capture.device.heimdall.gain_control = policy;
  const before = context(candidate);
  for (const [key, value] of [['gain', 7.7], ['channel_gains', [7.7, 8.7, 9.7, 10.7, 11.7]],
    ['gain_control', policy === 'fixed' ? 'measured_per_channel' : 'fixed']]) {
    candidate.capture.device.heimdall[key] = value;
    assert.equal(context(candidate), before, `${policy}: ${key} preserves geometry context`);
  }
}
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
for (const file of ['processing_capabilities.js', 'config_profiles.js', 'kraken_geometry.js', 'config_ui.js']) window.eval(fs.readFileSync(path.join(__dirname, '../../html/js', file), 'utf8') +
  (file === 'config_ui.js' ? `
    window.fixtureDraft = () => activeConfig;
    window.observeFixtureEdits = events => {
      const filter = refreshSettingsFilter, validate = scheduleValidation;
      refreshSettingsFilter = () => { events.push(['filter']); filter(); };
      scheduleValidation = () => { events.push(['validate']); validate(); };
      return () => { refreshSettingsFilter = filter; scheduleValidation = validate; };
    };` : ''));
const query = key => window.document.querySelector(`[data-path="${key}"] input, [data-path="${key}"] select`);
const mapping = () => query('capture.device.array_geometry.mapping_confirmed');
const message = () => window.document.getElementById('config-message').textContent;
function change(input, value) { input.value = String(value); input.dispatchEvent(new window.Event('input', {bubbles: true})); }
(async () => {
  try {
    await window.renderConfiguration();
    // Freeze the existing edit boundary before sharing scalar/composite ownership.
    const geometryNode = () => window.document.querySelector('.kraken-geometry');
    const hostPath = ['capture', 'device', 'heimdall', 'host'];
    const originalHost = saved.capture.device.heimdall.host;
    const originalContext = window.KrakenGeometry.context;
    const originalInvalidate = window.KrakenGeometry.invalidate;
    const events = [];
    window.KrakenGeometry.context = config => {
      events.push(['context', config.capture.device.heimdall.host]);
      return originalContext(config);
    };
    window.KrakenGeometry.invalidate = geometry => {
      events.push(['invalidate', geometry.mapping_confirmed]);
      return originalInvalidate(geometry);
    };
    for (const scalar of [true, false]) {
      const beforeNode = geometryNode();
      const nextHost = scalar ? 'scalar-fixture.local' : 'composite-fixture.local';
      events.length = 0;
      const stopObserving = window.observeFixtureEdits(events);
      if (scalar) window.setValue(hostPath, nextHost);
      else window.applyComposite(config => { config.capture.device.heimdall.host = nextHost; });
      stopObserving();
      assert.deepEqual(events, [['context', originalHost], ['context', nextHost], ['invalidate', true],
        ['filter'], ['validate']], 'Both edits invalidate, update filtering, then schedule validation');
      assert.equal(window.fixtureDraft().capture.device.array_geometry.mapping_confirmed, false);
      assert.equal(mapping().checked, !scalar, 'Only scalar edits immediately rebuild geometry controls');
      assert.equal(geometryNode() === beforeNode, !scalar, 'Composite edits retain the geometry DOM');
      assert.equal(window.document.getElementById('config-save').disabled, true,
        'An edit must await fresh validation before saving');
      assert.match(window.document.getElementById('config-state').textContent, /unsaved change/);
      window.document.getElementById('config-reset').click();
      assert.equal(window.fixtureDraft().capture.device.heimdall.host, originalHost);
      assert.equal(mapping().checked, true, 'Discard restores assertions before any receiver transaction');
    }
    window.KrakenGeometry.context = originalContext;
    window.KrakenGeometry.invalidate = originalInvalidate;
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
