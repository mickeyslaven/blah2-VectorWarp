'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const common = require('../../html/js/common.js');
const clone = value => JSON.parse(JSON.stringify(value));

(async () => {
  for (const kind of ['map', 'spectrum', 'timing', 'detection']) {
    const calls = [];
    const fetchUrls = [];
    const snapshotScopes = [];
    let update;
    let scheduled;
    let resized;
    const windowEvents = {};
    const running = {capture: {fs: 2400000}, process: {data: {cpi: .2},
      detection: {enable: true}}, truth: {adsb: {enabled: true}}};
    let detected = {timestamp: 1000, delay: [1], doppler: [2]};
    const truthAircraft = {abc123: {hex: 'abc123', flight: 'TEST1', alt_geom: 5000,
      delay: 1.3, doppler: 40}, def456: {hex: 'def456', flight: 'TEST2', alt_geom: 30000,
      delay: 2.5, doppler: -20}};
    let tracked = {timestamp: 1000, data: [
      {id: 9, state: 'ACTIVE', delay: 8, doppler: 2},
      {id: 10, state: 'COASTING', delay: 10, doppler: -3}
    ]};
    const context = vm.createContext({
      URL, URLSearchParams,
      performance: {now: () => 1000},
      clearTimeout() {}, setTimeout(callback) { resized = callback; return 1; },
      liveApiUrl: url => url,
      radarPresentationUrl: url => `${url}${url.includes('?') ? '&' : '?'}presentation=aligned`,
      getIlluminatorContext: async () => ({enabled: true, id: 'fm-90.5',
        item: {id: 'fm-90.5', output_sample_rate_hz: 300000}}),
      selectedIlluminatorUrl: (path, context) => {
        const url = new URL(path, 'http://vectorwarp.test');
        if (context?.id) url.searchParams.set('illuminator', context.id);
        return `${url.pathname}${url.search}`;
      },
      selectedIlluminatorConfig: config => ({...config,
        capture: {...config.capture, fs: 300000}}),
      urlMap: '/api/map', xTitle: 'Time', yTitle: 'Doppler', xVariable: 'timestamp', yVariable: 'doppler',
      getRadarRuntimeConfig: async () => running,
      aircraftLabel: common.aircraftLabel,
      aircraftIdentity: common.aircraftIdentity,
      radarVisualSemantics: common.radarVisualSemantics,
      aircraftPlotSymbol: common.aircraftPlotSymbol,
      aircraftPlotData: common.aircraftPlotData,
      aircraftHoverText: common.aircraftHoverText,
      aircraftHoverTemplate: common.aircraftHoverTemplate,
      VectorWarpPlotTheme: common.vectorwarpPlotTheme,
      stabilizeAircraftSnapshots: (rows, options) => {
        snapshotScopes.push(options?.scope);
        return common.stabilizeAircraftSnapshots(rows, options);
      },
      fetchRadarJson: async url => {
        fetchUrls.push(url);
        return url.startsWith('/api/detection') ? detected :
          url.startsWith('/api/tracker') ? tracked :
          url.startsWith('/api/adsb/delay-doppler') ? truthAircraft : new Promise(() => {});
      },
      window: {innerWidth: 1200, location: new URL('http://vectorwarp.test/display/map/'), addEventListener(name, callback) { windowEvents[name] = callback; }, clearTimeout() {},
        setTimeout(callback) { scheduled = callback; return 1; }},
      Plotly: Object.fromEntries(['react', 'update', 'relayout', 'restyle'].map(method =>
        [method, async (...args) => { calls.push({method, args: clone(args)}); }])),
      startRadarPlot: (_url, render) => { update = render; return {stop() {}}; }
    });
    require('./display-fixture')(context);
    const source = fs.readFileSync(path.join(__dirname, '../../html/js', `plot_${kind}.js`), 'utf8');
    assert.ok(!source.includes('setInterval'), `${kind} must use shared frame timing`);
    vm.runInContext(source, context);
    if (kind === 'spectrum' || kind === 'timing')
      assert.deepEqual(calls[0].args[1], [], `${kind}: waiting must not fabricate a heatmap`);
    if (kind !== 'map') {
      const initialLayout = calls[0].args[2], initialConfig = calls[0].args[3];
      assert.deepEqual(clone(initialLayout.margin), {l: 92, r: 50, b: 50, t: 10, pad: 0},
        `${kind} retains the shared plot viewport margins`);
      assert.equal(initialLayout.paper_bgcolor, 'rgba(0,0,0,0)',
        `${kind} retains the transparent plot canvas`);
      assert.equal(initialConfig.scrollZoom, kind === 'spectrum' ? undefined : true,
        `${kind} retains its original scroll-zoom behavior`);
    }
    if (context.vectorwarpMapReady) await context.vectorwarpMapReady;
    if (context.vectorwarpDetectionReady) await context.vectorwarpDetectionReady;
    if (context.vectorwarpSpectrumReady) await context.vectorwarpSpectrumReady;
    assert.equal(typeof update, 'function');
    if (kind === 'map') {
      const frameAt = (at, values = [[-40, -40]]) => ({timestamp: at,
        delay: [0, 1], doppler: [0], data: values, maxPower: -40});
      const hold = (at, values, rate) => context.applyClientMaxHold(frameAt(at, values),
        rate === undefined ? undefined : {max_hold_decay_db_per_second: rate}, at);
      const reset = () => { context.maxHoldState = null; context.window.vectorwarpMapMode = 'maxhold'; };
      const initial = frameAt(0, [[20, -10]]), immutable = clone(initial);
      const fade = step => {
        reset(); context.applyClientMaxHold(initial, undefined, 0);
        let result;
        for (let at = step; at <= 10000; at += step) result = hold(at);
        return result;
      };
      const slow = fade(1000), fast = fade(100);
      assert.ok(Math.abs(slow.data[0][0] - fast.data[0][0]) < 1e-10,
        'Equal elapsed time must produce equal decay at different frame rates');
      assert.deepEqual(clone(slow.data), [[10, -20]], 'dB fade decreases positive and negative cells');
      assert.equal(slow.maxPower, 10);
      assert.deepEqual(initial, immutable, 'Display hold must not mutate the radar frame');
      reset(); hold(0, [[20, -10]], .001);
      assert.ok(Math.abs(hold(7200000, undefined, .001).data[0][0] - 12.8) < 1e-10,
        'A two-hour hold loses only 7.2 dB at 0.001 dB/s');
      reset(); hold(0, [[20, -10]], 0);
      assert.deepEqual(clone(hold(7200000, undefined, 0).data), [[20, -10]], 'Zero retains peaks');
      assert.deepEqual(clone(hold(7201000, [[25, -5]], 0).data), [[25, -5]], 'New peaks replace held peaks');
      for (const rate of [undefined, null, '1', -1, 21, NaN, Infinity]) {
        reset(); hold(0, [[20, -10]], rate);
        assert.equal(hold(1000, undefined, rate).data[0][0], 19, 'Invalid display rate uses the bounded default');
      }
      reset(); hold(0, [[20, -10]], 1);
      assert.equal(hold(1000000, undefined, 1).data[0][0], -40, 'Paused tabs catch up to the current floor');
      for (const changed of [
        {...frameAt(1000), delay: [1, 2]}, {...frameAt(1000), doppler: [1]},
        frameAt(1000, [[-40]]), frameAt(-1)
      ]) {
        reset(); hold(0, [[20, -10]], 0);
        assert.equal(context.applyClientMaxHold(changed, {max_hold_decay_db_per_second: 0}, 1000).data[0][0],
          -40, 'Changed coordinates, shape or backwards frame time clears history');
      }
      reset(); hold(1000, [[20, -10]], 0);
      assert.equal(context.applyClientMaxHold(frameAt(2000), {}, 0).data[0][0], -40,
        'Backwards display time clears history');
      reset();
      context.applyClientMaxHold({...initial, timestamp: undefined}, {}, 0);
      assert.equal(context.applyClientMaxHold({...frameAt(1000), timestamp: undefined}, {}, 1000).data[0][0],
        19, 'Untimestamped frames still decay on monotonic display time');
      const display = {max_hold_decay_db_per_second: .001};
      assert.strictEqual(common.selectedIlluminatorConfig({...running, display},
        {enabled: true, id: 'test', item: {output_sample_rate_hz: 300000}}).display, display,
      'Selected lane configuration preserves the common display preference');
      reset(); hold(0, [[20, -10]], 0);
      context.configureMapChannel({id: 'another-lane'});
      assert.equal(hold(1000, undefined, 0).data[0][0], -40, 'Lane changes clear held peaks');
      context.configureMapChannel(await context.getIlluminatorContext());
      context.window.vectorwarpMapMode = undefined;
      assert.strictEqual(context.applyClientMaxHold(initial, {}, 0), initial, 'Live map remains unchanged');
      const mobile = context.delayDopplerPresentation(390);
      const desktop = context.delayDopplerPresentation(1200);
      const landscapePhone = context.delayDopplerPresentation(844, 390);
      assert.ok(mobile.margin.l + mobile.margin.r < desktop.margin.l + desktop.margin.r,
        'Phone layout must reserve materially more width for the delay-Doppler heatmap');
      assert.equal(mobile.xTitle, 'Range (km)');
      assert.equal(mobile.yTitle, 'Hz');
      assert.equal(mobile.colorbar.title.text, 'dB');
      assert.ok(mobile.colorbar.thickness < desktop.colorbar.thickness);
      assert.equal(mobile.showScale, false,
        'Phone layout must reserve the narrow viewport for radar data, not a color scale');
      assert.equal(desktop.showScale, true);
      assert.equal(landscapePhone.showScale, false);
      assert.equal(landscapePhone.xTitle, 'Range (km)');
      assert.equal(context.mapPresentationUrl('/api/map', 768),
        '/api/map?max_rows=128&max_cols=128',
        'Tablets request the bounded peak-preserving map representation');
      assert.equal(context.mapPresentationUrl('/api/map', 1280),
        '/api/map?max_rows=192&max_cols=256',
        'Desktop uses the bounded higher-detail display representation');
    }
    if (kind === 'spectrum') {
      const fallback = context.spectrumColorRange([[NaN, Infinity]]);
      assert.equal(fallback.zmin, -120);
      assert.equal(fallback.zmax, 0);
    }
    const frame = {
      map: {timestamp: 1000, nRows: 3, delay: [1], doppler: [1, 2, 3], data: [[1], [2], [3]], maxPower: 15},
      spectrum: {timestamp: [1000], frequency: [[100000, 100001]], spectrum: [[-92, -37]]},
      timing: {timestamp: [1000], captureEpoch: [0], captureFirstEpochSample: [8448789504],
        captureEndEpochSample: [8449989504], gpu_resident_ready: [15], detector: [5],
        cpi: [20], frameTimestamp: 1000},
      detection: {timestamp: [1000], delay: [1], doppler: [2], snr: [3]}
    }[kind];
    assert.notEqual(await update(clone(frame)), false);
    if (kind === 'detection') {
      const trace = calls.filter(call => call.method === 'react').at(-1).args[1][0];
      assert.equal(trace.name, common.radarVisualSemantics.labels.detection);
      assert.equal(trace.marker.color, common.radarVisualSemantics.colors.detection,
        'Standalone detection/history plots share the live DD detection color');
      assert.equal(trace.marker.symbol, 'circle');
    }
    if (kind === 'map') assert.ok(snapshotScopes.includes('delay-doppler:fm-90.5'),
      'ADS-B snapshot retention is isolated to the selected illuminator lane');
    assert.equal(calls.filter(call => call.method === 'react').length, 2, `${kind}: initial real frame must create correct traces without tearing down its plot root`);
    if (kind === 'map') {
      const initialTraces = calls.filter(call => call.method === 'react').at(-1).args[1];
      assert.equal(initialTraces[2].marker.symbol, 'circle',
        'ADS-B truth stays a circular overlay in delay–Doppler coordinates');
      assert.deepEqual(initialTraces[2].marker.color,
        Object.values(truthAircraft).map(common.aircraftAltitudeColor),
        'Aircraft altitude colors match the regular geographic Map');
      assert.equal(initialTraces[2].name, 'ADS-B position');
      assert.deepEqual([1, 3, 4, 2, 5].map(index => initialTraces[index].legendrank),
        [10, 20, 30, 40, 50], 'Detection, track, coast, aircraft, Remote ID keep one legend order');
      assert.equal(initialTraces[1].marker.symbol, 'circle',
        'Radar detections keep their established circular marker styling');
      assert.equal(initialTraces[1].marker.size, 8, 'Raw detections are small, light markers');
      assert.equal(initialTraces[3].marker.color, '#ff7a45',
        'Updated radar tracks render as strongly outlined orange markers');
      assert.equal(initialTraces[3].marker.symbol, 'circle');
      assert.equal(initialTraces[3].marker.size, 11,
        'Updated tracks are compact solid orange dots rather than bullseyes');
      assert.equal(initialTraces[3].x[0], 8 * 299792458 / 300000 / 1000,
        'Tracker sample bins use the selected lane output rate');
      assert.equal(initialTraces[4].marker.symbol, 'circle-open',
        'Coasting persistent tracks remain visible as hollow orange rings');
      assert.equal(initialTraces[4].x[0], 10 * 299792458 / 300000 / 1000);
      assert.ok(context.urlMap.includes('/api/radar/frame') && context.urlMap.includes('illuminator=fm-90.5'),
        'The atomic heatmap URL is resolved only after the selected lane is known');
      assert.ok(fetchUrls.some(url => url.includes('/api/detection') && url.includes('frame_timestamp=1000')));
      assert.ok(fetchUrls.some(url => url.includes('/api/tracker') && url.includes('frame_timestamp=1000')),
        'Both overlays are requested for the exact heatmap frame');
      assert.ok(fetchUrls.some(url => url.includes('/api/adsb/delay-doppler') &&
        url.includes('frame_timestamp=1000')),
      'ADS-B truth is projected to the exact same heatmap frame');
    }
    if (kind === 'spectrum') {
      const trace = calls.at(-1).args[1][0];
      assert.deepEqual(trace.x, [100, 100.001]);
      assert.equal(trace.zauto, false);
      assert.equal(trace.zmin, -92);
      assert.equal(trace.zmax, -37);
      assert.match(trace.colorbar.title.text, /dB/);
    }
    if (kind === 'timing') {
      const traces = calls.at(-1).args[1];
      assert.deepEqual(traces.map(trace => trace.name), ['Detector (ms)', 'Cpi (ms)']);
      assert.ok(traces.every(trace => Math.max(...trace.y) < 1000000),
        'Epoch and absolute sample counters must never be graphed as stage milliseconds');
    }
    const repeatStart = calls.length;
    await update(clone(frame));
    if (kind === 'map') {
      assert.ok(calls.slice(repeatStart).some(call => call.method === 'update'),
        'The map refreshes even when its independent marker request completes nearby');
    } else assert.equal(calls.at(-1).method, 'update');
    if (kind === 'spectrum') {
      assert.equal(calls.at(-1).args[1].zauto[0], false);
      assert.equal(calls.at(-1).args[1].zmin[0], -92);
      assert.equal(calls.at(-1).args[1].zmax[0], -37);
      await update({timestamp: [1001], frequency: [[100000, 100001]], spectrum: [[-73, -73]]});
      assert.equal(calls.at(-1).method, 'update');
      assert.equal(calls.at(-1).args[1].zmin[0], -78,
        'Flat disconnected-input history retains a non-degenerate negative-dB scale');
      assert.equal(calls.at(-1).args[1].zmax[0], -68);
      await update({timestamp: [1000, 1001], frequency: [[100000, 100001], [100000, 100001]],
        spectrum: [[-300, -90], [-80, -30]]});
      assert.equal(calls.at(-1).method, 'update');
      assert.equal(calls.at(-1).args[1].zmin[0], -110,
        'The numerical floor cannot flatten the useful 80 dB history window');
      assert.equal(calls.at(-1).args[1].zmax[0], -30);
      await update({timestamp: [1002], frequency: [[100000, 100001, 100002]], spectrum: [[-100, -60, -30]]});
      assert.equal(calls.at(-1).method, 'react', 'Spectrum topology changes reconcile the existing Plotly root');
      assert.ok(calls.at(-1).args[1][0].zmax < 0,
        'Negative dB spectrum data cannot render against Plotly default 0..1 bounds');
    }
    if (kind === 'timing') {
      await update({timestamp: [1001], cpi: [20], capture: [5], frameTimestamp: 1001});
      assert.equal(calls.at(-1).method, 'react', 'Timing stage changes reconcile the existing Plotly root');
    }
    if (kind === 'map') {
      const nextFrame = {...clone(frame), timestamp: 1001};
      assert.equal(await update(nextFrame), true,
        'An exact-frame overlay request must not make the heatmap retry and burst');
      const latestMapUpdate = calls.filter(call => call.method === 'update').at(-1);
      assert.deepEqual(latestMapUpdate.args[1].x[1], [1],
        'The matching raw detection is updated atomically with the heatmap');
      assert.equal(latestMapUpdate.args[1].x[3][0], 8 * 299792458 / 300000 / 1000,
        'The matching updated track is rendered in the same Plotly update');
      assert.equal(latestMapUpdate.args[1].x[4][0], 10 * 299792458 / 300000 / 1000,
        'A coasting track does not disappear between direct detections');
      assert.ok(fetchUrls.some(url => url.includes('frame_timestamp=1001')));
      context.radarOverlayMode = 'tracks';
      detected = {timestamp: 1002, delay: [99], doppler: [99]};
      await update({...clone(frame), timestamp: 1002});
      const tracksOnly = calls.filter(call => call.method === 'update').at(-1);
      assert.deepEqual(tracksOnly.args[1].x[1], [], 'Tracks-only mode hides raw detections');
      assert.ok(tracksOnly.args[1].x[3].length, 'Tracks-only mode preserves confirmed tracks');
      context.window.innerWidth = 390;
      context.window.innerHeight = 844;
      windowEvents.resize(); resized();
      const changedGrid = {...clone(frame), timestamp: 1002, delay: [1, 2], data: [[1, 2], [3, 4]]};
      const changedGridCallsBefore = calls.length;
      await update(changedGrid);
      assert.ok(calls.slice(changedGridCallsBefore).some(call => call.method === 'react'),
        'Map grid changes reconcile the existing Plotly root');
      const resizedGrid = calls.filter(call => call.method === 'react').at(-1).args;
      assert.deepEqual(resizedGrid[2].margin, {l: 38, r: 6, b: 38, t: 8, pad: 0},
        'A later grid redraw retains compact margins instead of restoring startup desktop layout');
      assert.equal(resizedGrid[2].xaxis.title.text, 'Range (km)');
      assert.equal(resizedGrid[2].yaxis.title.text, 'Hz');
      assert.equal(resizedGrid[1][0].showscale, false);
      assert.ok(!calls.some(call => call.method === 'newPlot'), 'Map never tears down the plot root');
      const events = {};
      const plot = {on: (name, callback) => { events[name] = callback; },
        addEventListener: (name, callback) => { events[name] = callback; }, setAttribute() {}};
      context.document = {getElementById: () => plot, querySelectorAll: () => []};
      context.bindDelayDopplerSelection();
      events.plotly_click({points: [{curveNumber: 2, pointNumber: 1}]});
      await context.refreshDelayDopplerSelection();
      assert.equal(context.selectedDelayDopplerTarget, 'aircraft:DEF456');
      const selectedStyle = clone(context.delayDopplerSelectionStyle(context.delayDopplerSelectionTraces[0]));
      assert.deepEqual(selectedStyle['marker.line.color'], ['#10251f', '#ffffff'],
        'White denotes selection without replacing altitude color or claiming radar support');
      assert.deepEqual(selectedStyle['marker.size'], [10, 14]);
      await update(changedGrid);
      assert.equal(context.selectedDelayDopplerTarget, 'aircraft:DEF456', 'Selection survives frame refresh');
      events.keydown({key: 'Escape'});
      assert.equal(context.selectedDelayDopplerTarget, null);
      events.plotly_click({points: [{curveNumber: 3, pointNumber: 0}]});
      assert.equal(context.selectedDelayDopplerTarget, 'track:9');
      context.configureMapChannel({id: 'another-channel'});
      assert.equal(context.selectedDelayDopplerTarget, null, 'Changing channel clears lane-local selection');
      context.window.vectorwarpMapMode = 'maxhold';
      running.display = {max_hold_decay_db_per_second: .001};
      const atomic = (timestamp, values) => ({schema: 'vectorwarp-radar-display-frame-v1',
        map: {timestamp, delay: [0, 1], doppler: [0], data: [values], maxPower: Math.max(...values)},
        detections: {delay: [], doppler: []}, tracks: {data: []}});
      context.performance.now = () => 0;
      await update(atomic(3000, [20, -10]));
      context.performance.now = () => 7200000;
      await update(atomic(3001, [-40, -40]));
      assert.ok(Math.abs(calls.filter(call => call.method === 'update').at(-1).args[1].z[0][0][0] - 12.8) < 1e-10,
        'The renderer applies the saved display rate through selected-lane runtime configuration');
      running.display.max_hold_decay_db_per_second = 0;
      context.performance.now = () => 10800000;
      await update(atomic(3002, [-40, -40]));
      assert.ok(Math.abs(context.maxHoldState.grid[0][0] - 12.8) < 1e-10,
        'A newly saved display rate applies to the next frame without restarting the view');
    }
  }
  const unavailableCalls = [];
  const unavailableContext = vm.createContext({
    URL, Date,
    liveApiUrl: value => value,
    radarPresentationUrl: value => value,
    VectorWarpPlotTheme: common.vectorwarpPlotTheme,
    getRadarRuntimeConfig: async () => ({process: {data: {cpi: .5},
      multi_illuminator: {enabled: true}}}),
    fetchRadarJson: async () => null,
    startRadarPlot: () => { throw new Error('Absent multi-mode IQ must not start an endless poll'); },
    document: {getElementById: () => ({setAttribute() {}})},
    Plotly: {react: async (...args) => unavailableCalls.push(args), update: async () => {}},
    window: {}
  });
  require('./display-fixture')(unavailableContext);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../../html/js/plot_spectrum.js'), 'utf8'),
    unavailableContext);
  await unavailableContext.vectorwarpSpectrumReady;
  assert.match(unavailableCalls.at(-1)[2].annotations[0].text, /wideband spectrum is unavailable/i,
    'Multi mode fails clearly when no global wideband spectrum is published');
  console.log('Plot first-frame/update, spectrum dB scale, frequency axis and late-stream tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
