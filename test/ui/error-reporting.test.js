'use strict';
const assert = require('node:assert/strict');
const {JSDOM} = require('./jsdom');
const reports = require('../../html/js/error_reporting');

const base = {serverId: 'server-1', configRevision: 'sha256:c0ffee', loadedRevision: 'sha256:c0ffee',
  radar: 'receiving', processorFresh: true, timestampConnections: 2,
  processor: {receiver: 'Kraken', state: 'live', error: '', sampleRate: 2400000,
    frequency: 527000000, channels: 5, totalSamples: 100, loops: 10},
  restart: {state: 'idle'}, acceleration: {active: 'vulkan'},
  system: {os: 'Linux', architecture: 'arm64', kernel: '6.12'},
  build: {version: '0.1.29', commit: 'a'.repeat(40), package: 'deb'},
  featureDiagnostics: {recording: {requested: true, paused: false, active: true,
    recordedSamples: 4800000, timelineCaptureAvailable: true, lowSpacePolicy: 'stop_recording'},
  replay: {active: false, paused: false, seekable: false, timelineAvailable: false},
  adsb: {enabled: true, online: true, degraded: false, sourceCount: 4, healthySourceCount: 3,
    aircraftCount: 42, directAircraftCount: 40, mlatAircraftCount: 2,
    associationLagCpis: 4, associationLagMs: 2000},
  processing: {acceleration: 'vulkan-resident', clutterAcceleration: 'vulkan-resident', configuredCpiMs: 500},
  presentation: {timestampConnections: 2, associationLagCpis: 4, associationLagMs: 2000}}};
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
  const relay = await import('../../services/report-relay/src/index.js');
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
    assert.equal(payload.runtime.configRevision, 'sha256:c0ffee');
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

  const hostile = ['Authorization: Basic dXNlcjpwYXNz', 'Authorization: "Bearer ghp_quoted"',
    'password=\\"correct horse battery staple\\"',
    '{\\"hex\\":\\"a1b2c3\\",\\"flight\\":\\"PRIVATE12\\",\\"icao24\\":\\"abc123\\",\\"flightNumber\\":\\"VW123\\",\\"call_sign\\":\\"SECRET7\\"}',
    '["safe","SECOND_SECRET"]', 'password=“correct horse battery staple”'].join(' ');
  const dirtyStatus = {...base, system: {...base.system, browser: hostile},
    processor: {...base.processor, model: hostile}};
  const dirty = reports.buildReport({incident: {code: 'MANUAL_REPORT', component: 'support',
    stage: 'manual', message: hostile, details: {routeName: ['status', 'SECOND_SECRET'],
      attemptCount: 2, icao24: 'abc123'}}, status: dirtyStatus, config, comment: hostile});
  const forbidden = /DO-NOT-SEND|192\.168\.1\.2|dXNlcjpwYXNz|ghp_quoted|correct horse|battery staple|a1b2c3|PRIVATE12|abc123|VW123|SECRET7|SECOND_SECRET/;
  assert.doesNotMatch(JSON.stringify(dirty), forbidden);
  assert.match(dirty.submissionId, /^[0-9a-f-]{36}$/);
  assert.deepEqual(reports.scalars({attemptCount: 2, lat: 1, lon: 2, latDeg: 1, lonDeg: 2,
    receiverSerialNumber: 'x', accessTokenValue: 'x', adsbHex: 'abc123', flight: 'VW1',
    icao: 'abc123', icao24: 'abc123', flightNumber: 'VW1', callsign: 'VW1', call_sign: 'VW1',
    aircraftHex: 'abc123', routeName: ['status', 'SECOND_SECRET']}), {attemptCount: 2});
  const normalized = relay.normalizeReport(dirty);
  const issue = relay.githubIssue(normalized, await relay.reportFingerprint(normalized),
    await relay.submissionReceiptId(normalized));
  assert.doesNotMatch(issue.body, forbidden);
  assert.deepEqual(normalized.runtime.details, {attemptCount: 2, lastFrameAgeMs: null,
    timestampConnections: 2, processorFresh: true});
  assert.equal(normalized.features.recording.timelineCaptureAvailable, true);
  assert.equal(normalized.features.adsb.mlatAircraftCount, 2);
  assert.equal(normalized.features.processing.configuredCpiMs, 500);
  assert.equal(normalized.features.presentation.associationLagMs, 2000);

  const legacyId = '123e4567-e89b-42d3-a456-426614174000';
  const legacy = {...dirty, submissionId: legacyId,
    incident: {...dirty.incident, message: hostile}, comment: hostile,
    system: {...dirty.system, browser: hostile}, receiver: {...dirty.receiver, model: hostile}};
  localStorage.clear(); document.getElementById('settings').textContent = '';
  let directBody = '';
  await reports.submit(legacy, async (_url, options) => { directBody = options.body; return {
    ok: true, json: async () => ({ok: true, reportId: 'direct-migration-id'})}; }, localStorage);
  assert.doesNotMatch(directBody, forbidden); assert.match(directBody, new RegExp(legacyId));
  localStorage.setItem(reports.STORAGE_KEY, JSON.stringify(legacy));
  let migratedBody = '';
  global.fetch = async (_url, options) => { migratedBody = options.body; return {
    ok: true, json: async () => ({ok: true, reportId: 'migrated-id'})}; };
  reports.addSettingsEntry(document.getElementById('settings'), () => {
    throw new Error('legacy retry must not rebuild from live context');
  });
  assert.equal(document.querySelector('.report-support button').textContent, 'Retry report');
  const migratedCache = localStorage.getItem(reports.STORAGE_KEY);
  assert.doesNotMatch(migratedCache, forbidden); assert.match(migratedCache, new RegExp(legacyId));
  document.querySelector('.report-support button').click();
  document.querySelector('.site-dialog-actions button').click();
  const migratedPreview = document.querySelector('.report-preview').textContent;
  assert.doesNotMatch(migratedPreview, forbidden); assert.match(migratedPreview, new RegExp(legacyId));
  [...document.querySelectorAll('.site-dialog-actions button')].at(-1).click();
  await new Promise(resolve => setImmediate(resolve));
  assert.doesNotMatch(migratedBody, forbidden); assert.match(migratedBody, new RegExp(legacyId));
  assert.equal(localStorage.getItem(reports.STORAGE_KEY), null);
  document.getElementById('report-dialog').remove();

  const tracker = {active: null, generations: new Map()};
  const failure = status({radar: 'stale', processor: {state: 'live'}});
  const target = document.getElementById('error'); target.textContent = '';
  let incident = reports.classify(failure, tracker);
  const first = reports.addContextAction(target, () => ({incident, status: failure, config}));
  incident = reports.classify(failure, tracker);
  assert.equal(reports.addContextAction(target, () => ({incident, status: failure, config})), first);
  const reworded = status({radar: 'stale', processor: {state: 'live', error: 'callback wording changed'}});
  incident = reports.classify(reworded, tracker);
  assert.equal(reports.addContextAction(target, () => ({incident, status: reworded, config})), first);
  assert.equal(incident._recoveryGeneration, 0);
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
