'use strict';
const assert = require('node:assert/strict');
const {JSDOM} = require('jsdom');
const reports = require('../../html/js/error_reporting');

const incidents = [
  ['SDR_ENUMERATION_FAILED', 'receiver', 'enumerate'], ['SDR_OPEN_FAILED', 'receiver', 'open'],
  ['SDR_STREAM_LOST', 'receiver', 'stream'], ['RECEIVER_SYNC_FAILED', 'receiver', 'sync'],
  ['GAIN_MONITOR_FAILED', 'receiver', 'gain-monitor'], ['PROCESSOR_START_FAILED', 'processor', 'start'],
  ['PROCESSOR_RUNTIME_FAILED', 'processor', 'runtime'], ['GPU_BACKEND_FAILED', 'processor', 'acceleration'],
  ['RESTART_FAILED', 'lifecycle', 'restart'], ['CONFIG_PERSISTENCE_FAILED', 'configuration', 'persist'],
  ['API_UNAVAILABLE', 'api', 'connect'], ['ADSB_SOURCE_FAILED', 'adsb', 'connect']
];
const status = {configRevision: 'sha256:test', radar: 'no-data', processorFresh: true,
  timestampConnections: 2, processor: {receiver: 'Kraken', state: 'error', error: 'stream failed',
    sampleRate: 2400000, frequency: 527000000, channels: 5}, restart: {state: 'idle'},
  acceleration: {active: 'vulkan'}, system: {os: 'Linux', architecture: 'arm64', kernel: '6.12'}};
const config = {capture: {fc: 527000000, fs: 2400000, device: {type: 'Kraken', channel_count: 5,
  serial: 'DO-NOT-SEND'}}, truth: {adsb: {source: 'http://192.168.1.2:30005'}}};

for (const [code, component, stage] of incidents) {
  const report = reports.buildReport({incident: {code, component, stage, message: 'host=private.local serial=ABC 2001:db8::1'}, status, config, now: 1});
  assert.equal(report.incident.code, code); assert.equal(report.receiver.type, 'Kraken');
  assert.equal(report.receiver.sampleRateHz, 2400000); assert.equal(report.receiver.centerFrequencyHz, 527000000);
  assert.equal(report.receiver.channelCount, 5); assert.equal(report.runtime.configRevision, 'sha256:test');
  assert.doesNotMatch(JSON.stringify(report), /DO-NOT-SEND|192\.168\.1\.2|private\.local|ABC|2001:db8/);
}
assert.equal(reports.classify({sdrplayStartup: {state: 'failed', message: 'open'}}).code, 'SDR_OPEN_FAILED');
assert.equal(reports.classify({receiverSynchronization: {applicationState: 'partial'}}).code, 'RECEIVER_SYNC_FAILED');
assert.equal(reports.classify({restart: {state: 'failed'}}).code, 'RESTART_FAILED');
assert.equal(reports.classify({processor: {state: 'error'}}).code, 'PROCESSOR_RUNTIME_FAILED');
assert.equal(reports.classify({acceleration: {state: 'error'}}).code, 'GPU_BACKEND_FAILED');
assert.equal(reports.classify({radar: 'receiving'}), null);

(async () => {
  const memory = new Map();
  const storage = {setItem: (key, value) => memory.set(key, value), getItem: key => memory.get(key) || null,
    removeItem: key => memory.delete(key)};
  const report = reports.buildReport({incident: {code: 'MANUAL_REPORT', component: 'support', stage: 'manual', message: 'test'}, status, config});
  const id = await reports.submit(report, async (url, options) => {
    assert.equal(url, reports.ENDPOINT); assert.equal(options.headers['X-VectorWarp-Report'], '1');
    assert.equal(memory.has(reports.STORAGE_KEY), true);
    return {ok: true, json: async () => ({ok: true, reportId: 'abc123'})};
  }, storage);
  assert.equal(id, 'abc123'); assert.equal(memory.has(reports.STORAGE_KEY), false);
  await assert.rejects(reports.submit(report, async () => ({ok: false, json: async () => ({error: 'offline'})}), storage), /offline/);
  assert.equal(memory.has(reports.STORAGE_KEY), true);

  const dom = new JSDOM('<!doctype html><body><main id="settings"></main></body>', {url: 'https://radar.local/'});
  global.window = dom.window; global.document = dom.window.document;
  Object.defineProperty(global, 'navigator', {value: dom.window.navigator, configurable: true});
  Object.defineProperty(global, 'localStorage', {value: dom.window.localStorage, configurable: true});
  reports.addSettingsEntry(document.getElementById('settings'), () => ({incident: {code: 'MANUAL_REPORT', component: 'support', stage: 'manual', severity: 'degraded', message: 'manual'}, status, config}));
  assert.match(document.body.textContent, /Having trouble with VectorWarp/);
  document.querySelector('.report-support button').click();
  assert.match(document.getElementById('report-dialog').textContent, /will not include your location/);
  assert.deepEqual([...document.querySelectorAll('.site-dialog-actions button')].map(button => button.textContent),
    ['View report', 'Copy report', 'Cancel', 'Send report']);
  document.querySelector('.site-dialog-actions button').click();
  assert.equal(document.querySelector('.report-preview').hidden, false);
  assert.equal(JSON.parse(document.querySelector('.report-preview').textContent).consent, true);
  document.querySelector('.site-dialog-actions button:nth-last-child(2)').click();

  const retained = {...report, incident: {...report.incident, code: 'SDR_STREAM_LOST'}};
  localStorage.setItem(reports.STORAGE_KEY, JSON.stringify(retained));
  document.getElementById('settings').textContent = '';
  reports.addSettingsEntry(document.getElementById('settings'), () => { throw new Error('retained report was not reused'); });
  assert.equal(document.querySelector('.report-support button').textContent, 'Retry report');
  document.querySelector('.report-support button').click();
  document.querySelector('.site-dialog-actions button').click();
  assert.equal(JSON.parse(document.querySelector('.report-preview').textContent).incident.code, 'SDR_STREAM_LOST');
  console.log('error reporting UI/client contract passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
