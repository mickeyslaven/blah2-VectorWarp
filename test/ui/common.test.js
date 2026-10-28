'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

function page(origin, apiOrigin = origin) {
  const calls = [];
  const storage = new Map();
  const labels = {'live-status': {innerHTML: ''}, 'adsb-status': {innerHTML: '', hidden: true},
    'processor-alert': {textContent: '', hidden: true},
    'receiver-device-alert': {textContent: '', hidden: true}};
  const state = {radar: 'receiving', adsbEnabled: false, errors: [], restart: {state: 'idle'},
    adsb: {online: true}, receivers: {setupRequired: false, receivers: []},
    failWrite: false, stallBody: false};
  const context = vm.createContext({
    URL, URLSearchParams, AbortController,
    document: {hidden: false, getElementById: id => labels[id] || null, querySelectorAll: () => []},
    window: {location: new URL(origin), innerWidth: 1280, setTimeout, clearTimeout,
      localStorage: {getItem: key => storage.get(key), setItem: (key, value) => storage.set(key, value)}},
    fetch: async (url, options = {}) => {
      const target = new URL(url, origin);
      calls.push({url: target.href, method: options.method || 'GET', headers: options.headers || {}});
      if (target.origin !== apiOrigin) return {ok: true, status: 200, text: async () => '<html>Static web server</html>'};
      if (options.method === 'PUT' && state.failWrite) throw new Error('Response lost');
      const status = {serverId: 'test-api', ...state};
      const data = target.pathname === '/api/system/status' ? status :
        target.pathname === '/api/adsb/status' ? state.adsb :
        target.pathname === '/api/receivers' ? state.receivers : {};
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
    await context.fetchStatusResource(context.liveApiUrl('/capture/status?summary=1'));
    assert.equal(calls.at(-1).url, apiOrigin + '/capture/status?summary=1');
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
  const commonSource = fs.readFileSync(require.resolve('../../html/js/common.js'), 'utf8');
  assert.doesNotMatch(commonSource, /setInterval\(refreshReceiverDevices/,
    'Shared radar/display headers must never schedule receiver host probes');
  assert.doesNotMatch(commonSource, /\n\s*refreshReceiverDevices\(\);\n/,
    'Shared radar/display headers must never launch receiver host probes on page load');
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
  test.context.window.blah2AdsbStatusAt = 0;
  await test.context.refreshLiveStatus();
  assert.equal(test.labels['adsb-status'].hidden, false);
  test.state.processorFresh = false;
  test.state.processor = null;
  assert.equal(test.labels['adsb-status'].hidden, false);
  test.state.adsbEnabled = true;
  test.state.adsb = {online: false, aircraft: {available: true}, delayDoppler: {available: false, message: 'Converter unavailable'}};
  test.context.window.blah2AdsbStatusAt = 0;
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['adsb-status'].innerHTML.endsWith('ADS-B PARTIAL'));
  assert.equal(test.labels['adsb-status'].title, 'Converter unavailable');
  test.state.adsb = {online: false};
  test.context.window.blah2AdsbStatusAt = 0;
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['adsb-status'].innerHTML.endsWith('ADS-B OFFLINE'));
  test.state.adsb = {online: true};
  test.context.window.blah2AdsbStatusAt = 0;
  await test.context.refreshLiveStatus();
  assert.ok(test.labels['adsb-status'].innerHTML.endsWith('ADS-B ONLINE'));
  assert.ok(!test.calls.some(call => call.url.endsWith('/api/timestamp')), 'Status must not treat an old timestamp as online');

  test.state.receivers = {setupRequired: true, receivers: [{type: 'Usrp', label: 'Ettus USRP',
    capabilities: {configured: false, detected: true}}]};
  await test.context.refreshReceiverDevices();
  assert.equal(test.labels['receiver-device-alert'].hidden, false);
  assert.match(test.labels['receiver-device-alert'].textContent, /Ettus USRP detected.*finish receiver setup/);
  test.state.receivers = {setupRequired: false, receivers: [
    {type: 'RspDuo', label: 'SDRplay RSPduo', capabilities: {configured: true, detected: true}},
    {type: 'Usrp', label: 'Ettus USRP', capabilities: {configured: false, detected: true}}]};
  await test.context.refreshReceiverDevices();
  assert.match(test.labels['receiver-device-alert'].textContent, /active receiver was not changed/);
  test.state.receivers.receivers.pop();
  await test.context.refreshReceiverDevices();
  assert.equal(test.labels['receiver-device-alert'].hidden, true);

  test.state.failWrite = true;
  await assert.rejects(test.context.fetchStatusResource('/api/config', {method: 'PUT'}), /Response lost/);
  assert.equal(test.calls.filter(call => call.method === 'PUT').length, 1, 'Never automatically retry a write');
  await test.context.fetchStatusResource('/api/config');
  test.state.stallBody = true;
  const discoveryCallsBeforeTimeout = test.calls.filter(call =>
    call.url.includes('/api/system/status')).length;
  await assert.rejects(test.context.fetchStatusResource('/api/config', {}, 20), /timed out/);
  test.state.stallBody = false;
  await test.context.fetchStatusResource('/api/config');
  assert.equal(test.calls.filter(call => call.url.includes('/api/system/status')).length,
    discoveryCallsBeforeTimeout,
    'A transient body timeout must not trigger an API endpoint rediscovery storm');
  assert.equal(test.context.recordingDuration(3723000), '01:02:03');
  const hover = test.context.aircraftHoverText({flight: 'TEST1', r: 'N123', t: 'C172',
    hex: 'abc123', alt_baro: 5200, gs: 101, track: 87, seen_pos: .4},
  {delay: 12.34, doppler: -56.7});
  assert.match(hover, /TEST1/);
  assert.match(hover, /12\.34 km/);
  assert.doesNotMatch(hover, /%\{/,
    'Radar aircraft hover text must contain values, never unresolved Plotly expressions');
  const nestedHover = test.context.aircraftHoverText({flight: 'NESTED', hex: 'abc123', position: {
    altitude_geom_ft: 12000, ground_speed_kt: 210, track_deg_true: 87,
    baro_rate_fpm: 512, category: 'A1', squawk: '1200', rssi_dbfs: -8.5, seen_pos_s: .2}});
  assert.match(nestedHover, /12,000 ft/);
  assert.match(nestedHover, /210 kt/);
  const rotorcraftIcon = test.context.aircraftSvgMarkup({category: 'A7'});
  assert.match(rotorcraftIcon, /viewBox="0 0 100 100"/);
  assert.match(rotorcraftIcon, /M10 8 90 70M90 8 10 70/,
    'Rotorcraft markers must retain the selected Minimal X rotor silhouette');
  assert.match(rotorcraftIcon, /v35h-6V59/,
    'Rotorcraft markers must retain the selected single straight tail boom');
  assert.equal(test.context.aircraftKind({t: 'GLID'}), 'glider');
  assert.match(test.context.aircraftSvgMarkup({t: 'GLID'}), /viewBox="0 0 100 100"/,
    'Gliders must use the distinct long-wing planform');
  assert.match(test.context.aircraftSvgMarkup({category: 'B6'}), /M29 83h42/,
    'Fixed-wing UAVs must retain the visible rear pusher propeller');
  // ResizeObserver fires during Mapbox initialization, before the first plot
  // finishes. Relayout at that point throws asynchronously inside Mapbox.
  const mapTest = page('http://radar.local:9876');
  const raf = [], afterPlot = [];
  let styleLoaded = false, observedResize, relayoutCalls = 0, width = 800;
  let finishResize, fullscreenClick, fullscreenButton;
  const panelClasses = new Set(), pageClasses = new Set();
  const classList = set => ({
    add: value => set.add(value), contains: value => set.has(value),
    toggle: (value, active) => active ? set.add(value) : set.delete(value)
  });
  const panel = {classList: classList(panelClasses), appendChild() {}};
  const visual = {closest: () => panel, layout: {},
    _fullLayout: {mapbox: {_subplot: {map: {isStyleLoaded: () => styleLoaded}}}},
    getBoundingClientRect: () => ({width, height: 600}),
    once: (event, callback) => { assert.equal(event, 'plotly_afterplot'); afterPlot.push(callback); }};
  Object.assign(mapTest.context.document, {
    getElementById: id => id === 'data' ? visual : null,
    querySelector: () => null,
    documentElement: {classList: classList(pageClasses)},
    body: {classList: classList(pageClasses)},
    createElement: () => (fullscreenButton = {addEventListener(event, callback) {
      if (event === 'click') fullscreenClick = callback;
    }}), addEventListener() {}
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
  await fullscreenClick();
  assert.ok(panelClasses.has('fullscreen-fallback'),
    'Mobile browsers without the Fullscreen API need an in-page fullscreen fallback');
  assert.equal(fullscreenButton.textContent, 'Exit fullscreen');
  await fullscreenClick();
  assert.ok(!panelClasses.has('fullscreen-fallback'));
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
