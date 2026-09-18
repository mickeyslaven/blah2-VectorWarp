'use strict';

// Run with jsdom installed in the test environment, never in the radar process.
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('jsdom');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('../../api/config-manager');
const {setupDefaults} = require('../../api/config-store');
const clone = value => JSON.parse(JSON.stringify(value));
const dom = new JSDOM('<div id="configuration"></div>', {
  url: 'http://127.0.0.1:3000/display/configuration/', runScripts: 'outside-only'
});
const window = dom.window;
let saved = setupDefaults();
let revision = 'initial';
let writes = 0;
let holdValidation = false;
let releaseValidation;
window.liveApiUrl = value => value;
window.fetchStatusResource = (url, options) => window.fetch(url, options);
window.rememberApiPort = () => {};
window.fetch = async (url, options = {}) => {
  let body;
  let status = 200;
  if (url === '/api/config/validate') {
    body = validateConfig(JSON.parse(options.body), saved);
    if (holdValidation) {
      holdValidation = false;
      await new Promise(resolve => { releaseValidation = resolve; });
    }
  } else if (url.startsWith('/api/config?')) {
    assert.equal(options.headers['If-Match'], `"${revision}"`);
    assert.equal(options.headers['X-VectorWarp-Receiver-Sync'], 'synchronize-v1');
    saved = JSON.parse(options.body);
    writes++;
    revision = `save-${writes}`;
    body = {ok: true, restarting: false, revision, message: 'Saved; restart manually.'};
  } else if (url === '/api/config') body = saved;
  else if (url === '/api/config/capabilities') body = {
    editable: true, restartAvailable: false, configRevision: revision,
    deviceProfiles: getDeviceProfiles().map(profile => ({...profile,
      liveAvailable: profile.type === 'Kraken'})), fieldRules: FIELD_RULES
  };
  else if (url === '/api/upstream/status') body = {supported: false, matched: false,
    message: 'Hardware is not verified.'};
  else if (url === '/api/system/status') body = {serverId: 'test',
    configRevision: revision, loadedRevision: revision, radar: 'no-data',
    message: 'No radar frames received.', errors: []};
  else throw new Error(`Unexpected URL: ${url}`);
  return {ok: status === 200, status, headers: {get: key => key === 'ETag' ? `"${revision}"` : null},
    json: async () => clone(body)};
};
window.eval(fs.readFileSync(path.join(__dirname, '../../html/js/config_ui.js'), 'utf8'));
const query = key => window.document.querySelector(`[data-path="${key}"]`);
function change(input, value, event = 'input') {
  input.value = value;
  input.dispatchEvent(new window.Event(event, {bubbles: true}));
}

(async () => {
  try {
    await window.renderConfiguration();
    await window.validateActiveConfiguration();
    const optionalSerial = query('capture.device.serial').querySelector('input');
    assert.equal(optionalSerial.value, '');
    assert.equal(optionalSerial.required, false, 'One unambiguous RSPduo does not require an explicit serial');
    assert.equal(optionalSerial.checkValidity(), true);
    assert.equal(query('capture.device.serial').querySelector('label').textContent, 'RSPduo serial');
    assert.equal(window.document.querySelectorAll('[role=tab]').length, 6);
    assert.equal(window.document.querySelector('[role=tab][data-tab=truth] strong').textContent, 'ADS-B');
    assert.equal(query('truth.adsb.enabled').querySelector('label').textContent, 'Show ADS-B');
    window.showMessage('Settings saved. Radar restarted; new frames received.', 'success', '<img src=x> RF not measured.');
    const savedMessage = window.document.getElementById('config-message');
    assert.equal(savedMessage.firstChild.textContent, 'Settings saved. Radar restarted; new frames received.');
    assert.equal(savedMessage.querySelector('summary').textContent, 'Details');
    assert.equal(savedMessage.querySelector('details').open, false, 'Diagnostics start collapsed');
    assert.match(savedMessage.querySelector('details p').textContent, /RF not measured/);
    assert.equal(savedMessage.querySelector('img'), null, 'Diagnostic details cannot inject HTML');
    window.showMessage('Connection lost.', 'error');
    assert.equal(savedMessage.querySelector('details'), null, 'An error clears old success details');
    assert.equal(window.document.getElementById('config-warnings'), null);
    const siteAdvice = window.document.querySelector('.config-advice');
    assert.ok(siteAdvice);
    assert.equal(siteAdvice.closest('[role=tabpanel]').id, 'settings-panel-location');
    assert.equal(window.document.querySelector('#settings-panel-capture .config-advice'), null);
    assert.ok(!/estimated radar memory|Hardware tuning limits|Saving valid settings/.test(window.document.body.textContent));
    assert.equal(window.document.querySelectorAll('#settings-panel-location select').length, 2);
    assert.equal(window.document.querySelectorAll('.config-group summary b').length, 0);
    window.document.querySelector('[data-site-role=tx][data-site-action=add]').click();
    const siteDialog = window.document.getElementById('site-dialog');
    assert.ok(siteDialog);
    assert.equal(siteDialog.querySelector('h2').textContent, 'Add transmitter location');
    assert.equal(siteDialog.querySelector('[name=latitude]').max, '90');
    const submitSite = () => siteDialog.querySelector('form').dispatchEvent(
      new window.Event('submit', {bubbles: true, cancelable: true}));
    for (const [key, value] of Object.entries({name: 'Test tower', latitude: 91, longitude: 2, altitude: 50}))
      change(siteDialog.querySelector(`[name=${key}]`), String(value));
    submitSite();
    assert.ok(window.document.getElementById('site-dialog'), 'Invalid coordinates must keep the dialog open');
    assert.equal(writes, 0);
    change(siteDialog.querySelector('[name=latitude]'), '1');
    submitSite();
    assert.equal(window.document.getElementById('site-dialog'), null);
    assert.equal(window.document.getElementById('site-select-tx').selectedOptions[0].textContent, 'Test tower');
    assert.equal(saved.location.tx.name, 'Set transmitter site', 'The dialog must not write the server config');
    window.document.querySelector('[data-site-role=rx][data-site-action=add]').click();
    window.document.querySelector('#site-dialog button[type=button]').click();
    assert.equal(window.document.getElementById('site-dialog'), null, 'Cancel closes without assigning a location');
    assert.equal(await window.validateActiveConfiguration(), true);
    assert.equal(window.document.querySelector('.config-advice'), null, 'Resolved site advice must disappear');
    assert.equal(query('process.data.overlap'), null);
    assert.equal(window.document.querySelector('[data-config-group="process.data"]').querySelectorAll('.config-field').length, 1);
    assert.ok(query('process.data.buffer').closest('[data-config-group="process.performance"]'));
    const lna = query('capture.device.lnaState').querySelector('select');
    assert.equal(lna.options.length, 10);
    const mhz = query('capture.fc').querySelector('input');
    change(lna, '9', 'change');
    change(mhz, '30');
    assert.equal(lna.value, '9', 'Retuning must not silently replace the selected gain');
    assert.match(lna.selectedOptions[0].textContent, /current value.*review/);
    assert.deepEqual([...lna.options].slice(0, -1).map(option => Number(option.value)), [0,1,2,3,4,5,6]);
    assert.equal(await window.validateActiveConfiguration(), false);
    change(lna, '0', 'change');
    assert.equal(await window.validateActiveConfiguration(), true);
    change(mhz, '1500');
    assert.deepEqual([...lna.options].map(option => Number(option.value)), [0,1,2,3,4,5,6,7,8]);
    change(mhz, '204');
    change(lna, '1', 'change');
    assert.equal(await window.validateActiveConfiguration(), true);
    for (const field of window.document.querySelectorAll('.config-field')) {
      assert.ok(field.querySelector('label').htmlFor, field.dataset.path);
      assert.ok(field.querySelector('small').textContent, field.dataset.path);
    }
    const frequency = query('capture.fc').querySelectorAll('input');
    const windowSize = query('process.tracker.initiate.N').querySelector('input, select');
    change(windowSize, '2', windowSize.tagName === 'SELECT' ? 'change' : 'input');
    assert.equal(await window.validateActiveConfiguration(), false);
    for (const name of ['M', 'N']) {
      const field = query(`process.tracker.initiate.${name}`);
      assert.equal(field.classList.contains('invalid'), true);
      assert.ok(!field.querySelector('.config-field-error').textContent.includes('process.tracker'));
    }
    change(windowSize, '5', windowSize.tagName === 'SELECT' ? 'change' : 'input');
    change(frequency[1], '1000');
    assert.equal(await window.validateActiveConfiguration(), false);
    assert.equal(window.document.getElementById('config-save').disabled, true);
    assert.equal(query('capture.fc').classList.contains('invalid'), true);
    change(frequency[1], '640');
    change(frequency[0], '');
    assert.equal(await window.validateActiveConfiguration(), false);
    change(frequency[0], '205');
    change(frequency[2], '123');
    assert.equal(await window.validateActiveConfiguration(), true);
    await window.saveConfiguration();
    assert.equal(saved.capture.fc, 205640123);
    assert.deepEqual(saved.location.tx, {name: 'Test tower', latitude: 1, longitude: 2, altitude: 50});
    assert.deepEqual(Object.keys(saved.location).sort(), ['rx', 'tx']);
    assert.equal(saved.location.rx.name, 'Set receiver site');
    assert.ok(JSON.parse(window.localStorage.getItem('blah2-recent-sites')).some(site => site.name === 'Test tower'));
    assert.equal(writes, 1);
    assert.equal(window.document.getElementById('config-save').disabled, true);

    // Existing remote endpoints stay selected and survive local discovery edits.
    const adsb = query('truth.adsb.tar1090');
    const sourceMode = adsb.querySelector('select');
    const sourceEndpoint = adsb.querySelector('input');
    const remoteEndpoint = saved.truth.adsb.tar1090;
    assert.equal(sourceMode.value, 'server');
    assert.equal(sourceEndpoint.hidden, false);
    assert.equal(sourceEndpoint.value, remoteEndpoint);
    change(sourceMode, 'auto', 'change');
    assert.equal(sourceEndpoint.hidden, true);
    assert.equal(await window.validateActiveConfiguration(), true);
    assert.equal(saved.truth.adsb.tar1090, remoteEndpoint, 'Changing source mode must not save automatically');
    change(sourceMode, 'local:readsb', 'change');
    assert.equal(await window.validateActiveConfiguration(), true);
    change(sourceMode, 'server', 'change');
    assert.equal(sourceEndpoint.hidden, false);
    assert.equal(sourceEndpoint.value, remoteEndpoint, 'Returning to server mode retains the typed endpoint');
    change(sourceEndpoint, 'https://other-device.example/tar1090');
    assert.equal(await window.validateActiveConfiguration(), true);
    window.document.getElementById('config-reset').click();
    assert.equal(query('truth.adsb.tar1090').querySelector('input').value, remoteEndpoint);
    assert.equal(await window.validateActiveConfiguration(), true);

    // A stale valid response must not unlock Save after a newer invalid edit.
    change(frequency[0], '206');
    holdValidation = true;
    const pending = window.validateActiveConfiguration();
    await Promise.resolve();
    change(frequency[0], '');
    releaseValidation();
    assert.equal(await pending, false);
    assert.equal(window.document.getElementById('config-save').disabled, true);
    window.document.getElementById('config-reset').click();
    assert.equal(await window.validateActiveConfiguration(), true);
    assert.equal(query('capture.fc').classList.contains('invalid'), false);

    // Selecting a named location updates the draft; discard restores YAML values.
    const receiverSelect = window.document.getElementById('site-select-rx');
    const tower = [...receiverSelect.options].find(option => option.textContent === 'Test tower');
    change(receiverSelect, tower.value, 'change');
    assert.equal(window.document.getElementById('site-select-rx').selectedOptions[0].textContent, 'Test tower');
    window.document.getElementById('config-reset').click();
    assert.equal(window.document.getElementById('site-select-rx').value, '');
    assert.equal(await window.validateActiveConfiguration(), true);
    window.document.querySelector('[data-site-role=tx][data-site-action=edit]').click();
    const editDialog = window.document.getElementById('site-dialog');
    assert.equal(editDialog.querySelector('[name=name]').value, 'Test tower');
    assert.equal(editDialog.querySelector('[name=latitude]').value, '1');
    change(editDialog.querySelector('[name=altitude]'), '75');
    editDialog.querySelector('form').dispatchEvent(new window.Event('submit', {bubbles: true, cancelable: true}));
    assert.ok(query('location.tx').textContent.includes('75 m'));
    window.document.getElementById('config-reset').click();
    assert.ok(query('location.tx').textContent.includes('50 m'));
    assert.equal(await window.validateActiveConfiguration(), true);

    window.switchDevice('HackRF');
    assert.equal(await window.validateActiveConfiguration(), false,
      'Placeholder serials must not pass validation');
    const serials = query('capture.device.serial').querySelectorAll('input');
    assert.equal(query('capture.device.serial').querySelector('label').textContent, 'HackRF serial numbers');
    change(serials[0], '0001');
    change(serials[1], '0002');
    const amps = query('capture.device.amp_enable').querySelectorAll('select');
    change(amps[0], 'true', 'change');
    change(amps[1], 'true', 'change');
    assert.equal(await window.validateActiveConfiguration(), true);
    await window.saveConfiguration();
    assert.deepEqual(saved.capture.device.serial, ['0001', '0002']);
    assert.deepEqual(saved.capture.device.amp_enable, [true, true]);
    assert.equal(query('capture.replay.state').querySelector('input').disabled, false);
    assert.equal(query('capture.replay.format').querySelector('select').value, 'auto');

    window.switchDevice('Kraken');
    const gainControl = query('capture.device.heimdall.gain');
    const gainMode = gainControl.querySelector('select');
    const manualGain = gainControl.querySelector('input');
    assert.equal(gainMode.value, 'keep');
    change(gainMode, 'manual', 'change');
    change(manualGain, '49.6');
    assert.equal(await window.validateActiveConfiguration(), true);
    change(manualGain, '51');
    assert.equal(await window.validateActiveConfiguration(), false);
    change(gainMode, '-1', 'change');
    assert.equal(manualGain.hidden, true);
    assert.equal(manualGain.disabled, true, 'Inactive invalid manual gain must not block automatic gain');
    assert.equal(await window.validateActiveConfiguration(), true);
    const restoredAutomatic = window.typedInput(-1, ['capture', 'device', 'heimdall', 'gain']);
    assert.equal(restoredAutomatic.querySelector('select').value, '-1');
    assert.equal(restoredAutomatic.querySelector('input').hidden, true,
      'Saved automatic gain must not reopen as manual -1 dB');
    change(gainMode, 'keep', 'change');
    const reference = query('process.reference_synthesis.mode');
    assert.equal(reference.closest('[role=tabpanel]').id, 'settings-panel-capture');
    assert.equal(reference.closest('[data-config-group="capture.device"]').querySelector('summary strong').textContent, 'KrakenSDR Suite V2');
    assert.equal(query('capture.device.reference_channel').closest('[data-config-group]').dataset.configGroup,
      'process.reference_synthesis');
    assert.equal(window.document.querySelectorAll('[data-path="process.reference_synthesis.mode"]').length, 1);
    assert.equal(window.document.getElementById('upstream-status').closest('[role=tabpanel]').id,
      'settings-panel-capture');
    change(query('process.reference_synthesis.analysis_samples').querySelector('input'), '0');
    assert.equal(await window.validateActiveConfiguration(), false);
    assert.equal(window.document.querySelector('[role=tab][data-tab=capture]').classList.contains('has-errors'), true);
    assert.equal(window.document.querySelector('[role=tab][data-tab=process]').classList.contains('has-errors'), false);
    change(query('process.reference_synthesis.analysis_samples').querySelector('input'), '32768');
    const compatibilityValues = Object.entries(FIELD_RULES).filter(([, rule]) => rule.readOnly)
      .map(([key]) => [key, key.split('.').reduce((value, part) => value?.[part], saved)]);
    change(query('process.data.cpi').querySelector('input'), '0.25');
    change(query('capture.device.channel_count').querySelector('select'), '8', 'change');
    assert.equal(query('capture.device.surveillance_channels').querySelectorAll('input').length, 8);
    assert.equal(await window.validateActiveConfiguration(), true);
    await window.saveConfiguration();
    assert.equal(saved.capture.device.channel_count, 8);
    assert.equal(saved.process.data.cpi, .25, 'The one frame setting writes the processor CPI');
    change(query('process.performance.fft_threads').querySelector('select'), '2', 'change');
    assert.equal(await window.validateActiveConfiguration(), true);
    await window.saveConfiguration();
    assert.equal(saved.process.performance.fft_threads, 2);
    change(query('process.performance.fft_threads').querySelector('select'), '0', 'change');
    assert.equal(await window.validateActiveConfiguration(), true);
    await window.saveConfiguration();
    assert.equal(saved.process.performance.fft_threads, 0, 'Auto must save numeric zero for the processor');
    for (const [key, value] of compatibilityValues)
      assert.deepEqual(key.split('.').reduce((item, part) => item?.[part], saved), value,
        `Hidden compatibility setting ${key} must survive browser saves`);
    assert.equal(saved.capture.device.heimdall.control_port, 8092);
    window.switchDevice('Usrp');
    assert.match(query('capture.device.type').querySelector('select').selectedOptions[0].textContent,
      /Ettus USRP \(replay only\)/);
    assert.match(window.document.getElementById('config-message').textContent, /replay only/i);
    assert.equal(await window.validateActiveConfiguration(), true);
    assert.equal(query('capture.device.antenna').querySelectorAll('input').length, 2);
    let restartChecks = 0;
    const ordinaryFetch = window.fetch;
    window.fetch = async url => {
      if (url !== '/api/system/status') return ordinaryFetch(url);
      restartChecks++;
      const state = {serverId: 'before', loadedRevision: 'old', radar: 'receiving',
        lastFrameAt: Date.now() + 100, timestampConnections: 1,
        restart: restartChecks === 1 ? {state: 'command-complete', timestampConnections: 1} :
          {state: 'failed', message: 'Mock restart failed'}};
      return {ok: true, json: async () => state};
    };
    await assert.rejects(window.monitorRadarRestart({serverId: 'before'}), /Mock restart failed/);
    assert.equal(restartChecks, 2, 'Old advancing frames must not be accepted as restart success');
    window.fetch = ordinaryFetch;
    for (const profile of getDeviceProfiles()) {
      window.switchDevice(profile.type);
      assert.equal(window.document.querySelectorAll('.config-group summary b').length, 0,
        `${profile.type}: field-count badges must not return`);
      const fields = [...window.document.querySelectorAll('.config-field')];
      const paths = fields.map(field => field.dataset.path);
      for (const key of ['surveillance_workers', 'fft_threads']) {
        const select = query(`process.performance.${key}`).querySelector('select');
        assert.equal([...select.options].find(option => option.value === '0').textContent, 'Auto',
          `${profile.type}: automatic ${key} must be selectable`);
      }
      for (const [key, rule] of Object.entries(FIELD_RULES)) {
        if (rule.readOnly) assert.equal(query(key), null, `${profile.type}: unused ${key} must not appear`);
      }
      assert.equal(window.document.querySelector('[data-config-group="truth.ais"]'), null);
      assert.equal(new Set(paths).size, paths.length, `${profile.type}: settings must appear only once`);
      const ids = [...window.document.querySelectorAll('[id]')].map(element => element.id);
      assert.equal(new Set(ids).size, ids.length, `${profile.type}: controls must have unique labels/IDs`);
      for (const field of fields) {
        const key = field.dataset.path;
        assert.ok(field.querySelector('label')?.htmlFor, `${profile.type}: ${key} needs an associated label`);
        if (FIELD_RULES[key]?.choices)
          assert.ok(field.querySelector('select'), `${profile.type}: ${key} should use a dropdown`);
      }
      for (const key of ['gain', 'gainReduction', 'gain_lna', 'gain_vga', 'serial', 'antenna', 'amp_enable']) {
        const field = query(`capture.device.${key}`);
        if (!field) continue;
        if (profile.type === 'RspDuo' && key === 'serial') {
          const serial = field.querySelector('input');
          assert.equal(serial.value, '', 'An unselected RSPduo has no serial restriction');
          assert.equal(serial.required, false, 'The serial is optional when one RSPduo is connected');
          assert.equal(field.querySelector('.array-inputs'), null,
            'One RSPduo serial identifies the device containing both tuner roles');
          continue;
        }
        assert.deepEqual([...field.querySelectorAll('.array-inputs label > span')].map(label => label.textContent),
          ['Reference', 'Surveillance'], `${profile.type}: ${key} must identify the two receiver roles`);
      }
      assert.equal(window.document.querySelectorAll('#settings-panel-location select').length, 2);
      assert.equal(window.document.querySelectorAll('#settings-panel-location [data-site-action=add]').length, 2);
      if (profile.type !== 'Kraken') assert.equal(query('process.reference_synthesis.mode'), null);
    }
    const previousGet = window.Storage.prototype.getItem;
    const previousSet = window.Storage.prototype.setItem;
    try {
      window.Storage.prototype.getItem = () => { throw new Error('Storage disabled'); };
      window.Storage.prototype.setItem = () => { throw new Error('Storage disabled'); };
      window.renderEditor();
      assert.ok(window.document.getElementById('site-select-tx').options.length > 1);
    } finally {
      window.Storage.prototype.getItem = previousGet;
      window.Storage.prototype.setItem = previousSet;
    }
    console.log('Browser DOM tests passed: all-device layout audit, site add/edit/cancel/select, grouped frequency, errors, stale validation, discard and save payloads.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
