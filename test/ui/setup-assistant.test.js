const assert = require('node:assert/strict');
const test = require('node:test');
const {JSDOM} = require('./jsdom');
const {render, recordingEstimate, searchWindow, gpuActive, receiverHardwareReady,
  wizardChecks} = require('../../html/js/setup_assistant.js');
const {configurationForReceiver} = require('../../api/config-store');
const {getDeviceProfiles} = require('../../api/config-manager');

test('recording estimates distinguish compact Kraken clips from portable recordings', () => {
  const config = {capture: {fs: 2_400_000, device: {type: 'Kraken', channel_count: 5},
    aircraft_clips: {pre_seconds: 2, post_seconds: 2}}};
  const estimate = recordingEstimate(config);
  assert.equal(estimate.manualPerSecond, 96_000_000);
  assert.equal(estimate.manualHour, 345_600_000_000);
  assert.equal(estimate.clipPerSecond, 24_000_000);
  assert.equal(estimate.clip, 96_000_000);
  assert.equal(estimate.clipLabel, 'Compact native 8-bit Kraken IQ');
});

test('automatic search window conservatively covers range and target speed', () => {
  const result = searchWindow({capture: {fs: 2_400_000, fc: 509_000_000}}, 50, 300);
  assert.equal(result.delayMax, 801);
  assert.equal(result.dopplerMax, 1020);
  assert.ok(Math.abs(result.kmPerBin - 0.124913524) < 1e-9);
});

test('automatic search window rejects incomplete setup', () => {
  assert.equal(searchWindow({capture: {}}, 50, 300), null);
});

test('GPU status recognizes both staged and fully resident Vulkan processing', () => {
  assert.equal(gpuActive({active: 'vulkan', state: 'ready'}), true);
  assert.equal(gpuActive({active: 'vulkan-resident', state: 'ready'}), true);
  assert.equal(gpuActive({active: 'cpu', state: 'ready'}), false);
});

test('assistant recognizes every supported SDR profile and refuses unavailable software', () => {
  for (const profile of getDeviceProfiles()) {
    if (profile.type === 'Unconfigured') continue;
    const config = configurationForReceiver(profile.type);
    if (profile.type === 'HackRF') config.capture.device.serial = ['abc123', 'def456'];
    config.location.rx = {name: 'Receiver', latitude: 1, longitude: 1, altitude: 1};
    config.location.tx = {name: 'Transmitter', latitude: 2, longitude: 2, altitude: 2};
    const readyProfile = {...profile, liveAvailable: true};
    assert.equal(wizardChecks(config, [readyProfile])[0].complete, true, profile.type);
    assert.equal(wizardChecks(config, [{...profile, liveAvailable: false}])[0].complete, false,
      `${profile.type} cannot finish when its live adapter is unavailable`);
  }
});

test('assistant identifies receiver-specific hardware requirements', () => {
  assert.equal(receiverHardwareReady({type: 'Kraken', heimdall: {
    host: '127.0.0.1', port: 8091, control_port: 8092}}), true);
  assert.equal(receiverHardwareReady({type: 'RspDuo', serial: ''}), true,
    'RSPduo may use the first discovered receiver');
  assert.equal(receiverHardwareReady({type: 'Usrp', address: 'type=b200', subdev: 'A:A A:B'}), true);
  assert.equal(receiverHardwareReady({type: 'HackRF', serial: ['abc123', 'def456']}), true);
  assert.equal(receiverHardwareReady({type: 'HackRF', serial: [
    'REFERENCE_DEVICE_SERIAL_NUMBER', 'SURVEILLANCE_DEVICE_SERIAL_NUMBER']}), false);
});

test('assistant opens a guided tabbed wizard, saves each step and applies helpers', async () => {
  const dom = new JSDOM('<div id="assistant"></div>');
  const config = {capture: {fc: 509_000_000, fs: 2_400_000,
    device: {type: 'Kraken', channel_count: 5, heimdall: {
      host: '127.0.0.1', port: 8091, control_port: 8092, gain_control: 'fixed'}},
    aircraft_clips: {pre_seconds: 2, post_seconds: 2}},
  process: {data: {cpi: .5}, detection: {enable: true}, tracker: {enable: true},
    ambiguity: {delayMin: 0, delayMax: 100, dopplerMin: -100, dopplerMax: 100},
    clutter: {followAmbiguity: false}}, location: {rx: {}, tx: {}}};
  const writes = [], modes = [], target = dom.window.document.getElementById('assistant');
  const api = {getConfig: () => config, mode: () => 'simple', setMode: value => modes.push(value),
    set: (path, value) => { writes.push([path.join('.'), value]);
      let cursor = config; for (const key of path.slice(0, -1)) cursor = cursor[key]; cursor[path.at(-1)] = value; },
    receiverProfiles: () => [{type: 'Kraken', label: 'KrakenSDR'}], switchReceiver() {},
    discoverReceivers: async () => [],
    saveStep: async () => true, save: async () => true,
    refresh: () => render(target, api), refreshEditor: () => render(target, api)};
  render(target, api);
  assert.equal(target.querySelectorAll('.setup-card').length, 0,
    'The assistant no longer duplicates settings as a card wall');
  assert.match(target.textContent, /essentials ready/i);
  target.querySelector('[data-mode="advanced"]').click();
  assert.deepEqual(modes, ['advanced']);
  target.querySelector('[data-start-setup]').click();
  await new Promise(resolve => setImmediate(resolve));
  assert.ok(target.querySelector('.setup-wizard'), 'Start setup opens a modal wizard');
  assert.equal(target.querySelectorAll('[data-wizard-step]').length, 6);
  assert.equal(target.querySelector('[data-wizard-step="0"]').disabled, false,
    'A completed receiver step remains directly selectable');
  assert.equal(target.querySelector('[data-wizard-step="1"]').disabled, false,
    'A completed tuning step remains directly selectable');
  assert.equal(target.querySelector('[data-wizard-step="3"]').disabled, false,
    'A completed radar step remains directly selectable even when an earlier site step is active');
  assert.equal(target.querySelector('[data-wizard-step="4"]').disabled, true,
    'Future steps cannot be skipped before current settings are saved');
  target.querySelector('[data-wizard-next]').click();
  await new Promise(resolve => setImmediate(resolve));
  assert.match(target.querySelector('.setup-wizard-content').textContent, /radar search/i);
  target.querySelector('[data-wizard-next]').click();
  assert.ok(writes.some(([path, value]) => path === 'process.ambiguity.delayMax' && value === 801));
  assert.ok(target.querySelector('.setup-wizard'), 'The next guided step remains in the wizard');
});

test('assistant automatically selects one discovered supported receiver', async () => {
  const dom = new JSDOM('<div id="assistant"></div>');
  const target = dom.window.document.getElementById('assistant');
  const config = configurationForReceiver('Usrp');
  config.capture.device.type = 'Unconfigured';
  let selected = null;
  const profiles = getDeviceProfiles().map(profile => ({...profile, liveAvailable: true}));
  const api = {getConfig: () => config, mode: () => 'simple', setMode() {}, set() {},
    receiverProfiles: () => profiles, discoverReceivers: async () => [
      {type: 'Usrp', label: 'Ettus USRP'}],
    switchReceiver: type => { selected = type; Object.assign(config,
      configurationForReceiver(type)); }, saveStep: async () => true, save: async () => true,
    refresh: () => render(target, api)};
  render(target, api);
  target.querySelector('[data-start-setup]').click();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(selected, 'Usrp');
  assert.equal(config.capture.device.type, 'Usrp');
  assert.match(target.textContent, /detected and selected automatically/i);
});

test('advanced mode collapses the beginner workflow instead of duplicating two settings systems', () => {
  const dom = new JSDOM('<div id="assistant"></div>');
  const target = dom.window.document.getElementById('assistant');
  render(target, {getConfig: () => ({capture: {device: {type: 'Kraken'}}}),
    mode: () => 'advanced', setMode() {}, openTab() {}, set() {}, refresh() {}, refreshEditor() {},
    receiverProfiles: () => [], discoverReceivers: async () => [],
    switchReceiver() {}, saveStep() {}, save() {}});
  assert.equal(target.querySelectorAll('.setup-card').length, 0);
  assert.match(target.textContent, /All receiver, DSP, service, and diagnostic settings are shown below/);
  assert.ok(target.querySelector('[data-mode="simple"]'), 'The operator can always return to Simple mode');
});
