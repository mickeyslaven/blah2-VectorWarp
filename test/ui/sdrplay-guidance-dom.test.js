'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('jsdom');
const {setupDefaults} = require('../../api/config-store');
const {getDeviceProfiles, FIELD_RULES, validateConfig} = require('../../api/config-manager');

const dom = new JSDOM('<div id="configuration"></div>', {
  url: 'http://127.0.0.1:3000/display/configuration/', runScripts: 'outside-only'
});
const window = dom.window;
const saved = setupDefaults();
saved.capture.device = {type: 'Kraken', channel_count: 5, reference_channel: 0,
  surveillance_channels: [0, 1, 2, 3, 4], heimdall: {host: '127.0.0.1', port: 8091, control_port: 8092}};
saved.capture.fs = 2400000;
let dependencyState = 'unknown', serviceState = 'unknown';
window.liveApiUrl = value => value;
window.rememberApiPort = () => {};
window.fetchStatusResource = (url, options) => window.fetch(url, options);
window.fetch = async (url, options = {}) => {
  let body;
  if (url === '/api/config') body = saved;
  else if (url === '/api/config/capabilities')
    body = {editable: true, deviceProfiles: getDeviceProfiles(), fieldRules: FIELD_RULES};
  else if (url === '/api/config/validate') body = validateConfig(JSON.parse(options.body), saved);
  else if (url === '/api/system/status') body = {radar: 'no-data', errors: []};
  else if (url === '/api/upstream/status') body = {supported: false};
  else if (url === '/api/receivers/discover') body = {
    configRevision: 'a'.repeat(64), buildCapabilitiesKnown: true, managementAvailable: true,
    receivers: [{type: 'RspDuo', label: 'SDRplay RSPduo',
      capabilities: {liveCompiled: true, runtimeLoadable: true},
      detection: {state: 'unknown'}, dependencies: {state: dependencyState},
      managedService: {required: true, state: serviceState},
      upstream: {availability: 'not-applicable'}, settings: [], setupGuide: []}],
    management: {actions: [{id: 'start-sdrplay', receiverType: 'RspDuo', kind: 'start-service',
      available: true, ready: false}]}, errors: []};
  else throw new Error(`Unexpected ${url}`);
  return {ok: true, status: 200, headers: {get: () => '"' + 'a'.repeat(64) + '"'},
    json: async () => JSON.parse(JSON.stringify(body))};
};
for (const file of ['kraken_geometry.js', 'config_ui.js'])
  window.eval(fs.readFileSync(path.join(__dirname, '../../html/js', file), 'utf8'));
const flush = () => new Promise(resolve => setImmediate(resolve));
const check = async () => {
  const button = [...window.document.querySelectorAll('#receiver-setup button')]
    .find(item => item.textContent === 'Check receiver software');
  assert.ok(button); button.click(); await flush(); await flush();
};

(async () => {
  try {
    await window.renderConfiguration();
    await check();
    const output = window.document.querySelector('#receiver-setup');
    const link = [...output.querySelectorAll('a')].find(item =>
      item.href === 'https://sdrplay.com/hardware-api/');
    assert.ok(link, 'Unknown SDRplay inventory must expose the official link immediately.');
    assert.match(output.textContent, /Could not check SDRplay API/);
    assert.equal([...output.querySelectorAll('button')].some(item =>
      item.textContent.startsWith('Setup guide:')), false,
    'The official link must not require opening the optional setup guide.');

    dependencyState = 'missing';
    await check();
    assert.match(output.textContent, /SDRplay API was not found/);
    assert.match(output.textContent, /accept its license locally/);
    assert.ok([...output.querySelectorAll('a')].some(item => item.href === 'https://sdrplay.com/hardware-api/'));

    dependencyState = 'installed'; serviceState = 'stopped';
    await check();
    assert.match(output.textContent, /SDRplay API is stopped/);
    assert.match(output.textContent, /Save & Restart starts the standard local service/);
    assert.ok([...output.querySelectorAll('button')].some(item =>
      item.textContent === 'Review Start SDRplay'),
    'Installed/stopped guidance must retain the existing reviewed start action.');

    serviceState = 'running';
    await check();
    assert.match(output.textContent, /SDRplay API is running; no service restart needed/);
    assert.equal([...output.querySelectorAll('a')].some(item => item.href === 'https://sdrplay.com/hardware-api/'), false,
      'An observed installed SDK must not be prompted for download.');
    Object.assign(saved, setupDefaults());
    await window.renderConfiguration();
    const startup = window.document.getElementById('receiver-startup');
    assert.match(startup.textContent, /No current RSPduo status/);
    const receipt = {schema: 1, receiver: 'RspDuo', status: 'accepted',
      hardwareVerified: false, readbackAvailable: false,
      requested: {...saved.capture.device, frequency: saved.capture.fc,
        sampleRate: saved.capture.fs, serial: '', ifFrequencyKhz: 1620,
        ifBandwidthKhz: 1536, decimation: 1},
      selected: {serial: '<img src=x onerror=alert(1)>'},
      sdk: {version: 3.15, stages: {open: true, init: true, gainUpdateA: true, gainUpdateB: true}}};
    const live = {radar: 'receiving', processorFresh: true,
      processor: {receiver: 'RspDuo', input: 'live', state: 'live', receiverStartup: receipt}};
    window.renderReceiverStartup(live);
    assert.match(startup.textContent, /SDRplay accepted settings; receiving radar frames/);
    assert.match(startup.textContent, /not measured tuner values/);
    assert.equal(startup.querySelector('img'), null, 'Receipt strings must be text, not HTML');
    assert.doesNotMatch(startup.textContent, /Running settings differ/);
    startup.querySelector('details').open = true;
    window.renderReceiverStartup(live);
    assert.equal(startup.querySelector('details').open, true, 'Polling must keep the details open');
    receipt.requested.frequency += 1000;
    window.renderReceiverStartup(live);
    assert.match(startup.textContent, /Running settings differ/);
    receipt.status = 'pending';
    window.renderReceiverStartup(live);
    assert.match(startup.textContent, /Applying settings/);
    live.processor.state = 'error'; live.processor.error = 'Init rejected settings';
    window.renderReceiverStartup(live);
    assert.match(startup.textContent, /Receiver error: Init rejected/);
    window.renderReceiverStartup({...live, processorFresh: false});
    assert.match(startup.textContent, /No current RSPduo status/);
    assert.equal(startup.querySelector('table'), null, 'Stale receipts must not appear current');
    window.renderReceiverStartup({...live, processor: {...live.processor, input: 'replay'}});
    assert.match(startup.textContent, /No current RSPduo status/);
    console.log('SDRplay guidance DOM fixture passed.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
