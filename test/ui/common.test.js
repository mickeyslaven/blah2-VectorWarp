'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

function page(origin, apiOrigin = origin) {
  const calls = [];
  const storage = new Map();
  const labels = {'live-status': {innerHTML: ''}, 'adsb-status': {innerHTML: '', hidden: true},
    'processor-alert': {textContent: '', hidden: true}};
  const state = {radar: 'receiving', adsbEnabled: false, errors: [], restart: {state: 'idle'},
    adsb: {online: true}, failWrite: false, stallBody: false};
  const context = vm.createContext({
    URL, URLSearchParams, AbortController,
    document: {getElementById: id => labels[id] || null, querySelectorAll: () => []},
    window: {location: new URL(origin), setTimeout, clearTimeout,
      localStorage: {getItem: key => storage.get(key), setItem: (key, value) => storage.set(key, value)}},
    fetch: async (url, options = {}) => {
      const target = new URL(url, origin);
      calls.push({url: target.href, method: options.method || 'GET', headers: options.headers || {}});
      if (target.origin !== apiOrigin) return {ok: true, status: 200, text: async () => '<html>Static web server</html>'};
      if (options.method === 'PUT' && state.failWrite) throw new Error('Response lost');
      const status = {serverId: 'test-api', ...state};
      const data = target.pathname === '/api/system/status' ? status :
        target.pathname === '/api/adsb/status' ? state.adsb : {};
      return {ok: true, status: 200, text: async () =>
        state.stallBody && target.pathname !== '/api/system/status' ? new Promise(() => {}) : JSON.stringify(data)};
    }
  });
  vm.runInContext(fs.readFileSync(require.resolve('../../html/js/common.js'), 'utf8'), context);
  return {context, state, calls, labels};
}

(async () => {
  for (const [origin, apiOrigin] of [
    ['http://192.168.1.20:49152', 'http://192.168.1.20:3000'],
    ['http://radar.local:49152', 'http://radar.local:3000'],
    ['http://[::1]:49152', 'http://[::1]:3000'],
    ['http://radar.local:9123', 'http://radar.local:9123'],
    ['https://radar.example', 'https://radar.example'],
    ['http://radar.local:3000', 'http://radar.local:3000']
  ]) {
    const {context, calls} = page(origin, apiOrigin);
    await context.fetchStatusResource(context.liveApiUrl('/api/config'));
    assert.equal(calls.at(-1).url, apiOrigin + '/api/config');
    await context.fetchStatusResource(context.liveApiUrl('/capture/status'));
    assert.equal(calls.at(-1).url, apiOrigin + '/capture/status');
    assert.ok(calls.every(call => !call.url.includes('/adsb2dd/')));
  }
  const explicit = page('http://radar.local:49152', 'http://radar.local:3456');
  explicit.context.window.location.search = '?apiPort=3456';
  await explicit.context.fetchStatusResource('/api/config');
  assert.equal(explicit.calls[0].url, 'http://radar.local:3456/api/system/status');
  await explicit.context.toggleRecording();
  const toggle = explicit.calls.find(call => call.url === 'http://radar.local:3456/capture/toggle');
  assert.equal(toggle.method, 'POST');
  assert.equal(toggle.headers['X-VectorWarp-Intent'], 'recording-toggle-v1');

  const test = page('http://radar.local:9876');
  for (const [radar, expected] of [['receiving', 'ONLINE'], ['stale', 'STALE'], ['no-data', 'OFFLINE']]) {
    test.state.radar = radar;
    await test.context.refreshLiveStatus();
    assert.ok(test.labels['live-status'].innerHTML.endsWith('RADAR ' + expected));
  }
  test.state.errors = ['Map port unavailable'];
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['live-status'].innerHTML.endsWith('RADAR ERROR'));
  assert.equal(test.labels['live-status'].title, 'Map port unavailable');
  test.state.errors = [];
  test.state.restart.state = 'running';
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['live-status'].innerHTML.endsWith('RADAR RESTARTING'));
  test.state.restart.state = 'idle';
  test.state.processorFresh = true;
  test.state.processor = {input: 'replay', state: 'playing', positionSamples: 25,
    totalSamples: 100, error: ''};
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['live-status'].innerHTML.endsWith('RADAR REPLAY PLAYING 25%'));
  test.state.processor = {...test.state.processor, state: 'complete'};
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['live-status'].innerHTML.endsWith('RADAR REPLAY COMPLETE'));
  test.state.processor = {...test.state.processor, state: 'error', error: 'Input is truncated'};
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['live-status'].innerHTML.endsWith('RADAR REPLAY ERROR'));
  assert.equal(test.labels['live-status'].title, 'Input is truncated');
  assert.equal(test.labels['processor-alert'].textContent, 'Input is truncated');
  assert.equal(test.labels['processor-alert'].hidden, false);
  test.state.processor = {input: 'live', state: 'error', error: 'Receiver stopped'};
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['live-status'].innerHTML.endsWith('RADAR INPUT ERROR'));
  assert.equal(test.labels['live-status'].title, 'Receiver stopped');
  test.state.processor = {input: 'replay', state: 'playing', positionSamples: 1, totalSamples: 2, error: ''};
  test.state.adsbEnabled = true;
  const adsbCallsBeforeReplay = test.calls.filter(call => call.url.endsWith('/api/adsb/status')).length;
  await test.context.refreshLiveStatus();
  assert.equal(test.labels['adsb-status'].hidden, true);
  assert.equal(test.calls.filter(call => call.url.endsWith('/api/adsb/status')).length, adsbCallsBeforeReplay,
    'Replay must not request a live ADS-B status');
  test.state.processor = {input: 'live', state: 'live', error: ''};
  await test.context.refreshLiveStatus();
  assert.equal(test.labels['adsb-status'].hidden, false);
  test.state.processorFresh = false;
  test.state.processor = null;
  assert.equal(test.labels['adsb-status'].hidden, false);
  test.state.adsbEnabled = true;
  test.state.adsb = {online: false, aircraft: {available: true}, delayDoppler: {available: false, message: 'Converter unavailable'}};
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['adsb-status'].innerHTML.endsWith('ADS-B PARTIAL'));
  assert.equal(test.labels['adsb-status'].title, 'Converter unavailable');
  test.state.adsb = {online: false};
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['adsb-status'].innerHTML.endsWith('ADS-B OFFLINE'));
  test.state.adsb = {online: true};
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['adsb-status'].innerHTML.endsWith('ADS-B ONLINE'));
  assert.ok(!test.calls.some(call => call.url.endsWith('/api/timestamp')), 'Status must not treat an old timestamp as online');

  test.state.failWrite = true;
  await assert.rejects(test.context.fetchStatusResource('/api/config', {method: 'PUT'}), /Response lost/);
  assert.equal(test.calls.filter(call => call.method === 'PUT').length, 1, 'Never automatically retry a write');
  test.state.stallBody = true;
  await assert.rejects(test.context.fetchStatusResource('/api/config', {}, 20), /timed out/);
  assert.equal(test.context.recordingDuration(3723000), '01:02:03');
  // ResizeObserver fires during Mapbox initialization, before the first plot
  // finishes. Relayout at that point throws asynchronously inside Mapbox.
  const mapTest = page('http://radar.local:9876');
  const raf = [], afterPlot = [];
  let styleLoaded = false, observedResize, relayoutCalls = 0, width = 800;
  let finishResize;
  const panel = {classList: {add() {}}, appendChild() {}};
  const visual = {closest: () => panel, layout: {},
    _fullLayout: {mapbox: {_subplot: {map: {isStyleLoaded: () => styleLoaded}}}},
    getBoundingClientRect: () => ({width, height: 600}),
    once: (event, callback) => { assert.equal(event, 'plotly_afterplot'); afterPlot.push(callback); }};
  Object.assign(mapTest.context.document, {
    getElementById: id => id === 'data' ? visual : null,
    querySelector: () => null,
    createElement: () => ({addEventListener() {}}), addEventListener() {}
  });
  Object.assign(mapTest.context.window, {
    addEventListener() {}, requestAnimationFrame: callback => raf.push(callback),
    ResizeObserver: class { constructor(callback) { observedResize = callback; } observe() { observedResize(); } },
    Plotly: {relayout: async (_element, dimensions) => {
      assert.equal(styleLoaded, true, 'Cannot resize before Mapbox has a style');
      relayoutCalls++; Object.assign(visual.layout, dimensions);
    }}
  });
  mapTest.context.addFullscreenControl();
  raf.shift()();
  assert.equal(relayoutCalls, 0);
  observedResize(); raf.shift()();
  assert.equal(afterPlot.length, 1, 'Only one resize should wait for the initial plot');
  styleLoaded = true; afterPlot.shift()(); raf.shift()();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(relayoutCalls, 1, 'Deferred resize must run after map initialization');
  observedResize(); raf.shift()();
  assert.equal(relayoutCalls, 1, 'Unchanged dimensions must not cause a relayout loop');
  mapTest.context.window.Plotly.relayout = (_element, dimensions) => {
    relayoutCalls++;
    Object.assign(visual.layout, dimensions);
    return new Promise(resolve => { finishResize = resolve; });
  };
  width = 900; observedResize(); raf.shift()();
  width = 1000; observedResize();
  assert.equal(raf.length, 0, 'Relayouts must not overlap');
  finishResize(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(raf.length, 1, 'A resize during relayout must be measured afterward');
  raf.shift()();
  assert.equal(visual.layout.width, 1000);
  finishResize(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(mapTest.context.window.blah2PlotResizeError, undefined);
  console.log('API discovery, proxy/hostname/IPv6 routing, body timeout and service freshness tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
