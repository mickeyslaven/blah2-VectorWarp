'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

function clockedPage(cpi = .5) {
  let now = 0;
  let sequence = 0;
  const timers = new Map();
  const requests = [];
  const state = {config: {process: {data: {cpi, overlap: .1}}}, frame: null, fail: false};
  const context = vm.createContext({
    URL, URLSearchParams, AbortController,
    Date: class extends Date { static now() { return now; } },
    window: {
      location: {hostname: '127.0.0.1', protocol: 'http:', port: '3000', search: ''},
      setTimeout(fn, delay) { timers.set(++sequence, {at: now + delay, fn}); return sequence; },
      clearTimeout(id) { timers.delete(id); }
    },
    fetch: async url => {
      requests.push(new URL(url, 'http://127.0.0.1:3000').pathname);
      if (state.fail) throw new Error('API restarting');
      const data = url.endsWith('/api/system/status') ? {serverId: 'fixture', radar: 'receiving'} :
        url.endsWith('/api/runtime/config') ? state.config : state.frame;
      const body = JSON.stringify(data);
      return {ok: true, text: async () => body};
    }
  });
  vm.runInContext(fs.readFileSync(require.resolve('../../html/js/common.js'), 'utf8'), context);
  async function settle() { for (let i = 0; i < 40; i++) await Promise.resolve(); }
  async function advance(ms) {
    const end = now + ms;
    await settle();
    let count = 0;
    for (;;) {
      const next = [...timers.entries()].sort((a, b) => a[1].at - b[1].at)[0];
      if (!next || next[1].at > end) break;
      assert.ok(count++ < 10000, 'Timers must remain bounded');
      now = next[1].at;
      timers.delete(next[0]);
      next[1].fn();
      await settle();
    }
    now = end;
    await settle();
  }
  return {context, state, requests, advance, settle, now: () => now};
}

(async () => {
  const page = clockedPage();
  const times = [];
  const loop = page.context.startRadarUpdates(() => { times.push(page.now()); });
  await page.advance(1500);
  assert.deepEqual(times, [0, 500, 1000, 1500]);
  assert.ok(page.requests.every(path => ['/api/runtime/config', '/api/system/status'].includes(path)), 'Never schedule from the saved/editor config');
  page.state.config = {process: {data: {cpi: .1, overlap: .09}}};
  await page.advance(700);
  assert.deepEqual(times.slice(-3), [2000, 2100, 2200], 'An open page follows restarted CPI without reload');
  loop.stop();
  await page.advance(5000);
  assert.equal(times.at(-1), 2200);

  const bounded = clockedPage(.05);
  const boundedTimes = [];
  const boundedLoop = bounded.context.startRadarUpdates(() =>
    boundedTimes.push(bounded.now()), 1000);
  await bounded.advance(2500);
  assert.deepEqual(boundedTimes, [0, 1000, 2000],
    'Heavy mobile views can impose a display cadence without changing radar CPI');
  boundedLoop.stop();

  const background = clockedPage(.2);
  const visibilityListeners = new Set();
  background.context.document = {
    hidden: true,
    addEventListener(name, listener) {
      if (name === 'visibilitychange') visibilityListeners.add(listener);
    },
    removeEventListener(name, listener) {
      if (name === 'visibilitychange') visibilityListeners.delete(listener);
    }
  };
  const backgroundTimes = [];
  const backgroundLoop = background.context.startRadarUpdates(() =>
    backgroundTimes.push(background.now()));
  await background.advance(3000);
  assert.deepEqual(backgroundTimes, [], 'Hidden tabs must not poll live radar frames');
  background.context.document.hidden = false;
  for (const listener of visibilityListeners) listener();
  await background.settle();
  assert.deepEqual(backgroundTimes, [3000], 'A visible tab resumes immediately without catch-up');
  backgroundLoop.stop();
  assert.equal(visibilityListeners.size, 0, 'Stopping a view releases its visibility listener');

  for (const cpi of [null, undefined, 0, -1, Infinity, NaN, '0.5'])
    assert.equal(page.context.radarFrameIntervalMs({process: {data: {cpi}}}), 1000);
  assert.equal(page.context.radarFrameIntervalMs({process: {data: {cpi: .001}}}), 16);
  assert.equal(page.context.radarFrameIntervalMs({process: {data: {cpi: 1e20}}}), 2147483647);

  const slow = clockedPage(.1);
  let calls = 0;
  let release;
  const slowLoop = slow.context.startRadarUpdates(() => {
    calls++;
    return new Promise(resolve => { release = resolve; });
  });
  await slow.advance(3000);
  assert.equal(calls, 1, 'Slow requests cannot overlap');
  release();
  await slow.settle();
  await slow.advance(16);
  assert.equal(calls, 1, 'A slow request must not trigger an immediate catch-up burst');
  await slow.advance(84);
  assert.equal(calls, 2, 'Resume with newest data on the next regular interval');
  slowLoop.stop();
  release();
  await slow.advance(5000);
  assert.equal(calls, 2);

  const plot = clockedPage(.2);
  const rendered = [];
  let renderFails = true;
  const plotLoop = plot.context.startRadarPlot('http://127.0.0.1:3000/api/map', data => {
    if (renderFails) { renderFails = false; throw new Error('render failed'); }
    rendered.push(data.timestamp);
  });
  await plot.advance(200);
  assert.equal(rendered.length, 0, 'No fake or empty frame is rendered');
  plot.state.frame = {timestamp: 100, data: [[1]]};
  await plot.advance(200);
  assert.equal(plot.context.window.blah2FrameError, 'render failed', 'Expose the current refresh failure');
  await plot.advance(400);
  assert.deepEqual(rendered, [100], 'Retry the same frame after a render error');
  assert.equal(plot.context.window.blah2FrameError, null, 'A successful refresh clears a recovered error');
  await plot.advance(400);
  assert.deepEqual(rendered, [100], 'Do not repaint unchanged data');
  plot.state.fail = true;
  await plot.advance(200);
  plot.state.fail = false;
  plot.state.frame = {timestamp: 200, data: [[2]]};
  await plot.advance(200);
  assert.deepEqual(rendered, [100, 200], 'Recover from network errors');
  plotLoop.stop();

  // Very long CPIs must not stop an already open page finding a faster restart.
  const long = clockedPage(600);
  const longTimes = [];
  const longLoop = long.context.startRadarUpdates(() => longTimes.push(long.now()));
  await long.advance(1000);
  long.state.config = {process: {data: {cpi: .2}}};
  await long.advance(1400);
  assert.deepEqual(longTimes, [0, 2000, 2200, 2400]);
  longLoop.stop();

  const lanes = clockedPage();
  const shared = lanes.context;
  shared.window.location.href = 'http://127.0.0.1:3000/display/locations';
  shared.liveApiUrl = path => path;
  let selection = {enabled: true, id: 'fm-90.5', item: {api_illuminator: true}};
  let scope = 'selected';
  shared.getIlluminatorContext = async () => selection;
  shared.illuminatorDisplayScope = () => scope;
  const reads = [];
  shared.fetchCachedJsonResource = async (url, options, cache) => {
    reads.push({url, options, cache}); return {timestamp: 1234};
  };
  assert.deepEqual(vm.runInContext('Object.keys(radarDisplayLanes)', shared),
    ['map', 'detection', 'timing', 'spectrum', 'geographic', 'network'],
    'Six adapters share one data-lane registry');
  for (const name of ['map', 'detection', 'timing', 'spectrum', 'geographic', 'network']) {
    await shared.readRadarDisplay(name, '/api/detection');
    const read = reads.at(-1), url = new URL(read.url, shared.window.location.href);
    assert.equal(url.searchParams.get('illuminator'),
      ['map', 'detection', 'geographic'].includes(name) ? selection.id : null);
    assert.equal(url.searchParams.get('presentation'), name === 'network' ? null : 'aligned');
    assert.equal(read.cache.awaitFresh, ['map', 'detection', 'timing', 'spectrum'].includes(name) ? true : undefined);
  }
  for (const path of ['/api/adsb', '/api/remote-id/delay-doppler?overlay=1', '/api/timing']) {
    await shared.readRadarDisplay('geographic', path, {awaitFresh: true, timestamp: 1234});
    const read = reads.at(-1), url = new URL(read.url, shared.window.location.href);
    assert.equal(url.searchParams.get('illuminator'), null, 'Global sources are never coerced into a selected lane');
    assert.equal(url.searchParams.get('frame_timestamp'), '1234');
    assert.equal(url.searchParams.get('presentation'), path.startsWith('/api/remote-id') ? null : 'aligned');
    assert.equal(read.cache.awaitFresh, true);
  }
  scope = 'aggregate';
  await shared.readRadarDisplay('geographic', '/api/tracker');
  assert.equal(new URL(reads.at(-1).url, shared.window.location.href).searchParams.has('illuminator'), false);
  scope = 'selected';
  selection = {...selection, item: {api_illuminator: false}};
  assert.equal(new URL(shared.radarDisplayUrl('detection', undefined, selection),
    shared.window.location.href).searchParams.has('illuminator'), false,
  'Synthetic primary catalogs do not fabricate a backend lane');
  for (const timestamp of [undefined, -1, .5, Infinity, 'bad'])
    assert.equal(shared.radarFrameUrl('/api/tracker', timestamp), '/api/tracker');

  let releaseRead;
  shared.fetchCachedJsonResource = () => new Promise(resolve => { releaseRead = resolve; });
  const changedRead = shared.readRadarDisplay('geographic', '/api/tracker');
  await lanes.settle();
  selection = {...selection, id: 'fm-103.3'};
  releaseRead({timestamp: 1234});
  await assert.rejects(changedRead, /Channel changed during refresh/,
    'An in-flight selected product cannot cross a channel switch');

  shared.fetchCachedJsonResource = async url => {
    if (url.includes('failed')) throw new Error('offline');
    return url.includes('/api/system/status') ?
      {radar: url.includes('fresh remote') ? 'degraded' : 'receiving',
        processorFresh: !url.includes('stale'), processor: {state: 'live'}} : {timestamp: 1234};
  };
  const network = await shared.readFederatedRadar('/api/live-scorecard', {nodes: [
    {id: 'local', local: true}, {id: 'fresh remote'}, {id: 'stale'}, {id: 'failed'}
  ]});
  assert.deepEqual(network.map(row => row.node.id), ['local', 'fresh remote'],
    'Network aggregation retains fresh live processors, including degraded nodes; partial transport failure is isolated');

  // Bundle contracts exercise actual alignment and failure isolation before
  // renderer math. These must remain independent of the page's DOM layout.
  const bundleReads = [], failedSources = new Set();
  const runtime = {truth: {adsb: {enabled: true}, remote_id: {enabled: true}}};
  const payloads = {
    '/api/tracker': {timestamp: 5678}, '/api/tracker/fused': {available: true},
    '/api/detection': {delay: [3]}, '/api/radar/coverage': {supporting_passes: 2},
    '/api/adsb': {aircraft: [{hex: 'AbC', lat: 40, lon: -80, seen_pos: 1},
      {hex: 'old', lat: 40, lon: -80, seen_pos: 120}, {hex: 'bad', lat: false, lon: -80}]},
    '/api/adsb/delay-doppler': {abc: {hex: 'abc', delay: 3, doppler: 4}},
    '/api/remote-id': {aircraft: [{hex: 'drone', lat: 40, lon: -80, remote_id: true}]},
    '/api/remote-id/delay-doppler': {drone: {delay: 5, doppler: 6}}
  };
  shared.getRadarRuntimeConfig = async () => runtime;
  shared.selectedIlluminatorConfig = config => ({...config, selected: true});
  selection = {...selection, item: {api_illuminator: true}};
  shared.window.vectorwarpIlluminatorContext = selection;
  shared.fetchCachedJsonResource = async (url, options, cache) => {
    const parsed = new URL(url, shared.window.location.href);
    bundleReads.push({url: parsed, cache});
    if (failedSources.has(parsed.pathname)) throw new Error(`offline ${parsed.pathname}`);
    return payloads[parsed.pathname] || {};
  };
  const geographic = await shared.readRadarDisplayBundle('geographic', {coverage: true, altitude: 'very_high'});
  assert.equal(geographic.config.selected, true);
  assert.equal(geographic.frameTimestamp, 5678);
  assert.equal(bundleReads.at(-1).url.searchParams.get('frame_timestamp'), '5678');
  const reach = bundleReads.find(read => read.url.pathname === '/api/radar/coverage');
  assert.equal(reach.url.searchParams.get('altitude'), 'very_high');
  assert.equal(reach.cache.awaitFresh, undefined, 'Coverage uses its canonical 30-second cache policy');
  bundleReads.length = 0;
  const joined = await shared.readRadarDisplayTruth(runtime, 5678);
  assert.equal(joined.aircraft.length, 2, 'Invalid and old positions are rejected');
  assert.equal(joined.aircraft[0].delay, 3, 'Truth identity joins are case-insensitive');
  assert.equal(joined.aircraft[0]._lane_geometry_current, true);
  assert.equal(joined.aircraft[1]._lane_geometry_current, false,
    'Remote ID never inherits another illuminator’s geometry');
  assert.ok(bundleReads.every(read => read.url.searchParams.get('frame_timestamp') === '5678' && read.cache.awaitFresh));
  assert.ok(!bundleReads.some(read => read.url.pathname === '/api/remote-id/delay-doppler'));
  failedSources.add('/api/remote-id');
  const partial = await shared.readRadarDisplayTruth(runtime, 5678);
  assert.equal(partial.aircraft.length, 1);
  assert.match(partial.aircraftError, /Some aircraft sources/);
  failedSources.add('/api/adsb');
  await assert.rejects(shared.readRadarDisplayTruth(runtime, 5678), /offline/);
  const degraded = await shared.readRadarDisplayBundle('network');
  assert.equal(degraded.adsbData, null, 'A failed optional network feed does not discard the fused result');
  assert.equal(bundleReads.find(read => read.url.pathname ===
    '/api/federation/live-aircraft-associations')?.cache.awaitFresh, true,
  'The network lane receives radar evidence from the coordinator-wide association feed');
  assert.ok(!bundleReads.some(read => read.url.pathname === '/api/live-aircraft-associations'),
    'The network lane never reduces federated evidence to the coordinator-local association feed');
  failedSources.clear();
  bundleReads.length = 0;
  await shared.readRadarDisplayBundle('network');
  assert.equal(bundleReads.find(read => read.url.pathname === '/api/adsb')?.url.searchParams.get('presentation'),
    'latest', 'The network display requests the coherent presentation population');
  selection.enabled = false;
  const singleTruth = await shared.readRadarDisplayTruth(runtime, 5678);
  assert.equal(singleTruth.aircraft[1]._lane_geometry_current, true, 'Single-site Remote ID geometry remains supported');
  payloads['/api/adsb'].disabledForReplay = true;
  assert.equal((await shared.readRadarDisplayTruth(runtime, 5678)).aircraft.length, 1,
    'Replay-disabled truth does not re-enter the geographic renderer');
  selection.enabled = true;

  const historyPage = clockedPage(), track = {id: 'F1', point: [1, 1], timestamp: 1000};
  const adapters = {project: item => item.point,
    model: {operationalTracks: result => result.available ? result.tracks : [], radarPoint: item => item.point}};
  const historyBundle = {config: {}, result: {available: true, tracks: [track]},
    adsbData: {now: 0, vectorwarp_presentation: {max_position_age_s: 5},
      aircraft: [{hex: 'ABC', point: [2, 2], gs: 100, track: 90}]},
    associationData: {associations: [{hex: 'abc', tracked: true}]}};
  historyPage.context.updateNetworkDisplayHistory(historyBundle, adapters);
  assert.equal(historyBundle.adsbAircraft[0].association.tracked, true);
  await historyPage.advance(1000);
  historyBundle.adsbData.aircraft = [];
  historyBundle.associationData.associations = [];
  track.point = [1.1, 1];
  historyPage.context.updateNetworkDisplayHistory(historyBundle, adapters);
  assert.equal(historyBundle.adsbAircraft[0].coasting, true);
  assert.equal(historyBundle.adsbAircraft[0].association, null, 'Coasting cannot preserve an expired radar badge');
  assert.equal(historyBundle.adsbAircraft[0].point[0], 2 + 100 * 1.852 / 3600);
  assert.equal(historyBundle.fusedHistory.get('F1').length, 1, 'Repeated measurement timestamps replace, never extend, a radar trail');
  track.timestamp = 2000;
  historyPage.context.updateNetworkDisplayHistory(historyBundle, adapters);
  assert.equal(historyBundle.fusedHistory.get('F1').length, 2);
  track.timestamp = 6000;
  historyPage.context.updateNetworkDisplayHistory(historyBundle, adapters);
  assert.equal(historyBundle.fusedHistory.get('F1').length, 1, 'Long radar measurement gaps break the trail');
  for (let index = 1; index <= 70; index++) {
    track.timestamp = 6000 + index * 100;
    await historyPage.advance(100);
    historyBundle.adsbData = {now: historyPage.now() / 1000,
      vectorwarp_presentation: {max_position_age_s: 5},
      aircraft: [{hex: 'ABC', point: [2, 2]}]};
    historyPage.context.updateNetworkDisplayHistory(historyBundle, adapters);
  }
  assert.equal(historyBundle.fusedHistory.get('F1').length, 20);
  assert.equal(historyBundle.adsbAircraft[0].trail.length, 60);

  const stalePage = clockedPage(), staleBundle = {config: {}, result: {available: true, tracks: []},
    adsbData: {now: 0, vectorwarp_presentation: {max_position_age_s: 5},
      aircraft: [{hex: 'STALE', point: [3, 3], seen_pos: 0}]}, associationData: {associations: []}};
  stalePage.context.updateNetworkDisplayHistory(staleBundle, adapters);
  await stalePage.advance(4900);
  stalePage.context.updateNetworkDisplayHistory(staleBundle, adapters);
  assert.equal(staleBundle.adsbAircraft.length, 1,
    'A brief source gap remains smooth inside the canonical presentation-age budget');
  await stalePage.advance(200);
  stalePage.context.updateNetworkDisplayHistory(staleBundle, adapters);
  assert.equal(staleBundle.adsbAircraft.length, 0,
    'Replaying an unchanged cached snapshot cannot renew its original observation age');
  assert.deepEqual(stalePage.context.clientJsonCachePolicy('/api/adsb?presentation=latest'),
    {freshMs: 0, staleMs: 0, storage: null},
    'The live network presentation never replays session-cached aircraft');
  const sourceBudgetPage = clockedPage(), sourceBudgetBundle = {config: {truth: {adsb: {max_position_age: 5}}},
    result: {available: true, tracks: []}, adsbData: {now: 7,
      vectorwarp_presentation: {max_position_age_s: 9.25},
      aircraft: [{hex: 'BUDGET', point: [4, 4], seen_pos: 7}]},
    associationData: {associations: []}};
  await sourceBudgetPage.advance(7000);
  sourceBudgetPage.context.updateNetworkDisplayHistory(sourceBudgetBundle, adapters);
  assert.equal(sourceBudgetBundle.adsbAircraft.length, 1,
    'A successful aircraft response carries its own source budget and never depends on a status request');
  historyBundle.result.available = false;
  historyBundle.adsbData.aircraft = [];
  await historyPage.advance(20001);
  historyPage.context.updateNetworkDisplayHistory(historyBundle, adapters);
  assert.equal(historyBundle.fusedHistory.size, 0, 'Unavailable fusion clears its history');
  assert.equal(historyBundle.adsbAircraft.length, 0, 'ADS-B coasting expires after twenty seconds');

  const coveragePage = clockedPage(), coverageReads = [];
  let coverageLane = {enabled: true, id: 'a'};
  coveragePage.context.window.location.href = 'http://127.0.0.1:3000/display/locations';
  coveragePage.context.getIlluminatorContext = async () => coverageLane;
  coveragePage.context.illuminatorDisplayScope = () => 'selected';
  coveragePage.context.liveApiUrl = path => path;
  coveragePage.context.fetchStatusResource = async url => {
    coverageReads.push(url);
    return {ok: true, text: async () => JSON.stringify({lane: new URL(url, 'http://localhost').searchParams.get('illuminator')})};
  };
  const readCoverage = () => coveragePage.context.readRadarDisplay('geographic', '/api/radar/coverage?hours=24&bins=72&altitude=all');
  assert.equal((await readCoverage()).lane, 'a');
  await coveragePage.advance(29000);
  await readCoverage();
  assert.equal(coverageReads.length, 1, 'One cache owns the thirty-second coverage lifetime');
  coverageLane = {enabled: true, id: 'b'};
  assert.equal((await readCoverage()).lane, 'b', 'Changing lanes cannot reuse another channel’s coverage');
  assert.equal(coverageReads.length, 2);

  const fixed = clockedPage(10), fixedTimes = [];
  fixed.context.document = {hidden: true};
  const fixedDisplay = await fixed.context.startRadarDisplay('network', () => fixedTimes.push(fixed.now()), {paused: true});
  assert.deepEqual(fixedTimes, []);
  fixedDisplay.start(); fixedDisplay.start();
  await fixed.advance(2100);
  assert.deepEqual(fixedTimes, [0, 1000, 2000], 'Fusion keeps its one-second clock even with a ten-second radar CPI');
  assert.equal(fixed.requests.length, 0, 'Fixed bundle polling does not acquire an extra runtime-config lane');
  fixedDisplay.stop(); fixedDisplay.stop();
  await fixed.advance(3000);
  fixedDisplay.start();
  await fixed.settle();
  assert.equal(fixedTimes.at(-1), 5100);
  fixedDisplay.stop();
  const blocked = clockedPage(), pendingCalls = [];
  const pendingDisplay = await blocked.context.startRadarDisplay('network', () =>
    new Promise(resolve => pendingCalls.push(resolve)));
  pendingDisplay.stop(); pendingDisplay.start(); pendingDisplay.refresh();
  await blocked.advance(3000);
  assert.equal(pendingCalls.length, 1, 'Pause/resume and manual refresh cannot overlap an unfinished bundle');
  pendingCalls[0](); await blocked.settle();
  await blocked.advance(1000);
  assert.equal(pendingCalls.length, 2);
  pendingDisplay.stop(); pendingCalls[1]();
  const paced = clockedPage(10), beats = [];
  let finishFirst;
  const pacedDisplay = await paced.context.startRadarDisplay('network', () => {
    beats.push(paced.now());
    if (beats.length === 1) return new Promise(resolve => { finishFirst = resolve; });
  });
  await paced.advance(1500); finishFirst(); await paced.settle();
  await paced.advance(500);
  assert.deepEqual(beats, [0, 2000], 'A slow fusion read skips missed beats without drifting the one-second clock');
  pacedDisplay.stop();

  const mapPage = clockedPage(), graph = {dataset: {}}, maps = [], saved = new Map();
  const viewport = {key: 'site-a', center: {lat: 40, lon: -80}, zoom: 10};
  const camera = {key: 'site-a', center: [-81, 41], zoom: 12, pitch: 30, bearing: 70};
  saved.set('vectorwarp-map-camera-v1', JSON.stringify(camera));
  mapPage.context.localStorage = {getItem: key => saved.get(key), setItem: (key, value) => saved.set(key, value)};
  mapPage.context.readRadarDisplay = async () => ({packs: []});
  mapPage.context.window.vectorwarpMapLibreReady = Promise.resolve({Map: class {
    constructor(options) {this.options = options; this.events = {}; maps.push(this);}
    once(name, callback) {this.events[name] = callback;}
    on(name, callback) {this.events[name] = callback;}
    getCenter() {return {lng: -82, lat: 42};} getZoom() {return 11;}
    getPitch() {return 20;} getBearing() {return 80;}
    remove() {this.removed = true;}
  }});
  let backgroundSyncs = 0;
  const openMap = () => mapPage.context.ensureRadarDisplayMap('geographic', graph,
    viewport, manifest => ({version: 8, manifest}), () => backgroundSyncs++);
  const firstMap = openMap(), sameMap = openMap();
  assert.equal(firstMap, sameMap, 'Concurrent refreshes share one MapLibre initialization promise');
  await mapPage.settle();
  assert.equal(maps.length, 1);
  assert.deepEqual(maps[0].options.center, camera.center);
  assert.equal(maps[0].options.pitch, 30);
  maps[0].events.load();
  assert.equal(await firstMap, maps[0]);
  assert.equal(await openMap(), maps[0]);
  assert.equal(backgroundSyncs, 1, 'Refresh does not reset the camera or repeat initialization');
  maps[0].events.moveend();
  assert.deepEqual(JSON.parse(saved.get('vectorwarp-map-camera-v1')).center, [-82, 42]);
  const failedGraph = {dataset: {}};
  const failedMap = mapPage.context.ensureRadarDisplayMap('geographic', failedGraph, viewport, () => ({}), () => {});
  await mapPage.settle();
  maps[1].events.error({error: new Error('style failed')});
  await assert.rejects(failedMap, /style failed/);
  assert.equal(maps[1].removed, true);
  assert.equal(failedGraph._vectorwarpMapReady, undefined, 'Failed initialization is released for a clean retry');

  const events = {}, configured = [];
  let starts = 0, stops = 0;
  shared.window.addEventListener = (name, handler) => { events[name] = handler; };
  shared.startRadarPlot = () => { starts++; return {stop() {stops++;}}; };
  const controller = await shared.startRadarDisplay('detection', () => {}, {
    configure: context => { configured.push(context.id); }
  });
  assert.deepEqual(configured, ['fm-103.3']);
  assert.equal(starts, 1);
  controller.start();
  assert.equal(starts, 1, 'Repeated start cannot create a second polling loop');
  events['vectorwarp:illuminator-change']({detail: {id: 'new-lane'}});
  assert.deepEqual(configured, ['fm-103.3', 'new-lane']);
  controller.stop(); controller.stop();
  assert.equal(stops, 1, 'Pause releases exactly one scheduler');
  controller.start();
  assert.equal(starts, 2, 'Resume creates one new scheduler');
  controller.stop();

  const freshSpectrum = {timestamp: [0], frequency: [[100000]], spectrum: [[-50]]};
  for (const [first, cpi, accepted] of [
    [freshSpectrum, .5, true], [{...freshSpectrum, timestamp: 0}, .5, true],
    [{...freshSpectrum, timestamp: [20]}, .5, false],
    [{...freshSpectrum, timestamp: [20]}, 20, true],
    [{...freshSpectrum, timestamp: ['bad']}, .5, false],
    [{...freshSpectrum, timestamp: [Infinity]}, .5, false],
    [{...freshSpectrum, frequency: [[]]}, .5, false],
    [{...freshSpectrum, spectrum: null}, .5, false], [null, .5, false]
  ]) {
    let rendered = 0, unavailable = 0;
    const beforeStarts = starts;
    shared.getRadarRuntimeConfig = async () => ({process: {data: {cpi}, multi_illuminator: {enabled: true}}});
    shared.fetchRadarJson = async () => first;
    const stream = await shared.startRadarDisplay('spectrum', () => {rendered++;},
      {unavailable: () => {unavailable++;}});
    assert.equal(rendered, accepted ? 1 : 0);
    assert.equal(unavailable, accepted ? 0 : 1);
    assert.equal(starts - beforeStarts, accepted ? 1 : 0,
      'Unsupported or stale wideband publication cannot start a polling loop');
    stream?.stop();
  }
  for (const name of ['map', 'detection', 'spectrum', 'timing']) {
    const source = fs.readFileSync(require.resolve(`../../html/js/plot_${name}.js`), 'utf8');
    assert.match(source, /startRadarDisplay\(/, `${name} uses the canonical display lifecycle`);
    assert.doesNotMatch(source, /startRadarPlot\(|getIlluminatorContext\(\)\.then|searchParams\.set\(['"](?:illuminator|presentation|frame_timestamp)/,
      `${name} cannot fork selection, alignment or startup ownership`);
  }
  const insights = fs.readFileSync(require.resolve('../../html/js/insights.js'), 'utf8');
  assert.doesNotMatch(insights, /async function (?:fetchJson|apiJson|federatedNodeJson)\(/,
    'Insight adapters must not recreate their retired parallel transport');
  assert.doesNotMatch(insights, /radarCoverageCache|async function loadTruth|new maplibre\.Map/,
    'Geographic adapters cannot recreate coverage, truth acquisition, or map lifecycle ownership');
  const fusion = fs.readFileSync(require.resolve('../../html/js/fusion_scope.js'), 'utf8');
  assert.doesNotMatch(fusion, /fetchRadarJson|setInterval|updateInFlight|adsbHistory|adsbTrailHistory|history\.push/,
    'Fusion renderer cannot recreate acquisition, polling, or temporal state');
  console.log('Shared frame cadence, restart, slow request and frame retry tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
