'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('./jsdom');
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
        ifBandwidthKhz: 1536, decimation: 1, sampleCounterScalePolicy: 'auto',
        sampleCounterScale: 3},
      selected: {serial: '<img src=x onerror=alert(1)>'},
      sdk: {version: 3.15, stages: {open: true, init: true, gainUpdateA: true, gainUpdateB: true}}};
    const live = {radar: 'receiving', processorFresh: true,
      processor: {receiver: 'RspDuo', input: 'live', state: 'live', receiverStartup: receipt}};
    window.renderReceiverStartup(live);
    assert.match(startup.textContent, /SDRplay accepted settings; receiving radar frames/);
    assert.match(startup.textContent, /not measured tuner values/);
    assert.match(startup.textContent, /Sample counter policy \/ effective ratioauto \/ 3/);
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

    Object.assign(saved, setupDefaults());
    saved.capture.device = {type: 'Usrp', address: 'type=b200', subdev: 'A:A A:B',
      antenna: ['RX2', 'RX2'], gain: [10, 20]};
    saved.capture.fs = 2000000;
    await window.renderConfiguration();
    const usrpTarget = window.document.getElementById('receiver-startup');
    assert.match(usrpTarget.textContent, /No current USRP status/);
    const usrpReceipt = {schema: 1, receiver: 'Usrp', status: 'accepted',
      hardwareVerified: true, readbackAvailable: true, physicalRfVerified: false,
      requested: {address: 'type=b200', subdevices: 'A:A A:B',
        frequency: saved.capture.fc, sampleRate: saved.capture.fs,
        antennas: ['RX2', 'RX2'], gains: [10, 20]},
      appliedReadback: {subdevices: 'A:A A:B',
        perChannelFrequencyHz: [saved.capture.fc, saved.capture.fc],
        perChannelSampleRateHz: [saved.capture.fs, saved.capture.fs],
        perChannelGainDb: [10, 20], perChannelAntenna: ['RX2', 'RX2']},
      coherence: {motherboard: 'B210', clockSource: 'internal',
        timeSource: 'internal', deviceIdentity: {motherboardSerial: '31ABC42',
          rxIds: ['B210 RX', '<img src=x onerror=alert(1)>']}}};
    const usrpLive = {radar: 'receiving', processorFresh: true,
      processor: {receiver: 'Usrp', input: 'live', state: 'live',
        receiverStartup: usrpReceipt}};
    window.renderReceiverStartup(usrpLive);
    assert.match(usrpTarget.textContent, /UHD read back both channels; receiving radar frames/);
    assert.match(usrpTarget.textContent, /device-API getter values/);
    assert.match(usrpTarget.textContent, /serial 31ABC42/);
    assert.match(usrpTarget.textContent, /Exact event-time scoring remains unavailable/);
    assert.equal(usrpTarget.querySelector('img'), null, 'USRP receipt strings must be text, not HTML');
    assert.doesNotMatch(usrpTarget.textContent, /Running settings differ/);
    usrpReceipt.requested.gains[1] = 21;
    window.renderReceiverStartup(usrpLive);
    assert.match(usrpTarget.textContent, /Running settings differ/);
    usrpLive.processor.state = 'error';
    usrpLive.processor.error = 'Automatic requalification exceeded the bounded discontinuity budget.';
    window.renderReceiverStartup(usrpLive);
    assert.match(usrpTarget.textContent, /Reconnect the USRP.*run vectorwarp restart/,
      'A terminal USRP error must tell the operator how to recover after reconnecting it.');

    Object.assign(saved, setupDefaults());
    saved.capture.device = {type: 'HackRF',
      serial: ['a1', 'b2'], gain_lna: [16, 32], gain_vga: [20, 40],
      amp_enable: [false, true]};
    saved.capture.fs = 6000000;
    await window.renderConfiguration();
    const hackrfTarget = window.document.getElementById('receiver-startup');
    assert.match(hackrfTarget.textContent, /No current HackRF status/);
    const hackrfReceipt = {schema: 1, receiver: 'HackRF', status: 'accepted',
      hardwareVerified: false, readbackAvailable: false, physicalRfVerified: false,
      setterAcceptance: true,
      requested: {frequency: saved.capture.fc, sampleRate: saved.capture.fs,
        serials: ['a1', 'b2'], lnaGainDb: [16, 32], vgaGainDb: [20, 40],
        ampEnabled: [false, true]},
      derived: {basebandFilterPolicy: 'libhackrf-sample-rate-auto-at-most-0.75-fs',
        independentReadback: false},
      deviceIdentity: {complete: true,
        resolvedSerials: ['000000000000000000000000000000a1',
          '<img src=x onerror=alert(1)>'], roles: ['reference', 'surveillance']},
      synchronization: {policy: 'shared-clock-hardware-trigger-v1',
        clockOutputRole: 'surveillance', hardwareTriggerRole: 'surveillance',
        startOrder: 'surveillance-then-reference', physicalWiringVerified: false,
        hardwareTimestampAvailable: false}};
    const hackrfLive = {radar: 'receiving', processorFresh: true,
      processor: {receiver: 'HackRF', input: 'live', state: 'live',
        receiverStartup: hackrfReceipt}};
    window.renderReceiverStartup(hackrfLive);
    assert.match(hackrfTarget.textContent, /accepted settings; receiving provisional paired frames/);
    assert.match(hackrfTarget.textContent, /setter acceptance, not RF or synchronization readback/);
    assert.match(hackrfTarget.textContent, /at most 0.75 × sample rate/);
    assert.match(hackrfTarget.textContent, /Pairing and event time remain provisional/);
    assert.equal(hackrfTarget.querySelector('img'), null,
      'HackRF receipt strings must be text, not HTML');
    assert.doesNotMatch(hackrfTarget.textContent, /Running settings differ/);
    hackrfReceipt.requested.lnaGainDb[1] = 24;
    window.renderReceiverStartup(hackrfLive);
    assert.match(hackrfTarget.textContent, /Running settings differ/);
    console.log('SDRplay guidance DOM fixture passed.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
