'use strict';
const assert = require('node:assert/strict');
const {JSDOM} = require('jsdom');
const reports = require('../../html/js/error_reporting');

const base = {serverId: 'server-1', configRevision: 'sha256:test', loadedRevision: 'sha256:test',
  radar: 'receiving', processorFresh: true, timestampConnections: 2,
  processor: {receiver: 'Kraken', state: 'live', error: '', sampleRate: 2400000,
    frequency: 527000000, channels: 5, totalSamples: 100, loops: 10},
  restart: {state: 'idle'}, acceleration: {active: 'vulkan'},
  system: {os: 'Linux', architecture: 'arm64', kernel: '6.12'},
  build: {version: '0.1.29', commit: 'a'.repeat(40), package: 'deb'}};
const config = {capture: {fc: 527000000, fs: 2400000, device: {type: 'Kraken', channel_count: 5,
  serial: 'DO-NOT-SEND'}}, truth: {adsb: {source: 'http://192.168.1.2:30005'}}};
const status = value => ({...base, ...value, processor: value.processor === null ? null :
  {...base.processor, ...(value.processor || {})}});
const cases = [
  ['SDR_ENUMERATION_FAILED', status({radar: 'no-data', processor: {state: 'error', error: 'Permission denied', totalSamples: 0,
    receiverStartup: {selected: null, sdk: {stages: {open: true, apiVersion: true, enumerate: false}}}}})],
  ['SDR_OPEN_FAILED', status({radar: 'no-data', sdrplayStartup: {state: 'failed', message: 'Device busy'}})],
  ['SDR_STREAM_LOST', status({radar: 'stale', lastFrameAt: 1, processor: {state: 'live', loops: 22}})],
  ['RECEIVER_SYNC_FAILED', status({receiverSynchronization: {applicationState: 'partial', rollbackAttempted: true}})],
  ['GAIN_MONITOR_FAILED', status({gainMonitor: {status: 'error', phase: 'probe', error: {code: 'TIMEOUT', message: 'Probe timed out'}, monitor: {consecutiveUnsafe: 2}}})],
  ['PROCESSOR_START_FAILED', status({radar: 'no-data', processor: {state: 'error', error: 'startup failed', totalSamples: 0, loops: 0}})],
  ['PROCESSOR_RUNTIME_FAILED', status({processor: {state: 'error', error: 'worker failed', totalSamples: 100, loops: 10}})],
  ['GPU_BACKEND_FAILED', status({acceleration: {state: 'error', active: 'cpu', reason: 'Vulkan failed'}})],
  ['RESTART_FAILED', status({restart: {state: 'failed', message: 'restart helper failed', exitStatus: 1}})],
  ['ADSB_SOURCE_FAILED', status({adsb: {enabled: true, online: false, sources: [
    {healthy: false, consecutive_failures: 3, message: 'ADS-B source timed out'}]}})]
];
const procedural = [
  ['CONFIG_PERSISTENCE_FAILED', reports.classifyFailure('config-persistence', {message: 'write uncertain', details: {writeAttempted: true}})],
  ['API_UNAVAILABLE', reports.classifyFailure('api', {message: 'status unavailable', details: {routeName: 'status'}})]
];

(async () => {
  const dom = new JSDOM('<!doctype html><body><main id="settings"></main><div id="error"></div></body>', {url: 'https://radar.local/'});
  global.window = dom.window; global.document = dom.window.document;
  Object.defineProperty(global, 'navigator', {value: dom.window.navigator, configurable: true});
  Object.defineProperty(global, 'localStorage', {value: dom.window.localStorage, configurable: true});
  Object.defineProperty(global, 'crypto', {value: require('node:crypto').webcrypto, configurable: true});

  for (const [expected, shape] of cases) {
    const incident = reports.classify(shape, {active: null, generations: new Map()});
    assert.equal(incident.code, expected);
    const target = document.getElementById('error'); target.textContent = '';
    reports.addContextAction(target, () => ({incident, status: shape, config}));
    target.querySelector('button').click();
    document.querySelector('.site-dialog-actions button').click();
    const payload = JSON.parse(document.querySelector('.report-preview').textContent);
    assert.equal(payload.incident.code, expected);
    assert.equal(payload.receiver.sampleRateHz, 2400000);
    assert.equal(payload.runtime.configRevision, 'sha256:test');
    assert.ok(Object.keys(payload.runtime.details).length > 0, `${expected} needs pathway evidence`);
    document.querySelector('.site-dialog-actions button:nth-last-child(2)').click();
  }
  for (const [expected, incident] of procedural) {
    const target = document.getElementById('error'); target.textContent = '';
    reports.addContextAction(target, () => ({incident, status: base, config})); target.querySelector('button').click();
    document.querySelector('.site-dialog-actions button').click();
    assert.equal(JSON.parse(document.querySelector('.report-preview').textContent).incident.code, expected);
    document.querySelector('.site-dialog-actions button:nth-last-child(2)').click();
  }

  const dirty = reports.buildReport({incident: {code: 'MANUAL_REPORT', component: 'support', stage: 'manual',
    message: '"serial":"ABC" Authorization: Bearer ghp_bad api_key=key tcp://private.local /Volumes/a /mnt/b \\\\server\\share'}, status: base, config});
  assert.doesNotMatch(JSON.stringify(dirty), new RegExp('DO-NOT-SEND|192\\.168\\.1\\.2|ABC|ghp_bad|private\\.local|Volumes|mnt/b|server'));
  assert.match(dirty.submissionId, /^[0-9a-f-]{36}$/);
  assert.deepEqual(reports.scalars({safeCount: 2, lat: 1, lon: 2, receiverSerialNumber: 'x',
    accessTokenValue: 'x', adsbHex: 'abc123', flight: 'VW1'}), {safeCount: 2});

  const tracker = {active: null, generations: new Map()};
  const failure = status({radar: 'stale', processor: {state: 'live'}});
  const target = document.getElementById('error'); target.textContent = '';
  let incident = reports.classify(failure, tracker);
  const first = reports.addContextAction(target, () => ({incident, status: failure, config}));
  incident = reports.classify(failure, tracker);
  assert.equal(reports.addContextAction(target, () => ({incident, status: failure, config})), first);
  assert.equal(target.querySelectorAll('.report-context-action').length, 1);
  reports.classify(status({radar: 'receiving'}), tracker);
  incident = reports.classify(failure, tracker);
  const second = reports.addContextAction(target, () => ({incident, status: failure, config}));
  assert.notEqual(second, first); assert.equal(incident._recoveryGeneration, 1);

  localStorage.clear(); document.getElementById('settings').textContent = '';
  let sends = 0;
  global.fetch = async () => ++sends === 1 ? {ok: false, json: async () => ({error: 'offline'})} :
    {ok: true, json: async () => ({ok: true, reportId: 'stable-id'})};
  reports.addSettingsEntry(document.getElementById('settings'), () => ({incident: {
    code: 'MANUAL_REPORT', component: 'support', stage: 'manual', severity: 'degraded', message: 'manual'}, status: base, config}));
  document.querySelector('.report-support button').click();
  [...document.querySelectorAll('.site-dialog-actions button')].at(-1).click();
  await new Promise(resolve => setImmediate(resolve));
  const retained = JSON.parse(localStorage.getItem(reports.STORAGE_KEY));
  assert.equal(retained.incident.code, 'MANUAL_REPORT');
  document.getElementById('report-dialog').remove();
  document.getElementById('settings').textContent = '';
  reports.addSettingsEntry(document.getElementById('settings'), () => { throw new Error('must reuse retained report'); });
  assert.equal(document.querySelector('.report-support button').textContent, 'Retry report');
  document.querySelector('.report-support button').click();
  [...document.querySelectorAll('.site-dialog-actions button')].at(-1).click();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(sends, 2); assert.equal(localStorage.getItem(reports.STORAGE_KEY), null);
  assert.match(document.querySelector('.report-state').textContent, /stable-id/);
  console.log('error reporting UI/client contract passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
