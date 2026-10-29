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
    let update;
    let scheduled;
    const running = {capture: {fs: 2400000}, process: {data: {cpi: .2},
      detection: {enable: true}}, truth: {adsb: {enabled: true}}};
    let detected = {timestamp: 1000, delay: [1], doppler: [2]};
    let tracked = {timestamp: 1000, data: [
      {id: 9, state: 'ACTIVE', delay: 8, doppler: 2},
      {id: 10, state: 'COASTING', delay: 10, doppler: -3}
    ]};
    const context = vm.createContext({
      URL,
      liveApiUrl: url => url,
      radarPresentationUrl: url => `${url}${url.includes('?') ? '&' : '?'}presentation=aligned`,
      urlMap: '/api/map', xTitle: 'Time', yTitle: 'Doppler', xVariable: 'timestamp', yVariable: 'doppler',
      getRadarRuntimeConfig: async () => running,
      aircraftLabel: common.aircraftLabel,
      aircraftPlotSymbol: common.aircraftPlotSymbol,
      aircraftPlotData: common.aircraftPlotData,
      aircraftHoverText: common.aircraftHoverText,
      aircraftHoverTemplate: common.aircraftHoverTemplate,
      stabilizeAircraftSnapshots: common.stabilizeAircraftSnapshots,
      fetchRadarJson: async url => {
        fetchUrls.push(url);
        return url.startsWith('/api/detection') ? detected :
          url.startsWith('/api/tracker') ? tracked : new Promise(() => {});
      },
      window: {innerWidth: 1200, location: new URL('http://vectorwarp.test/display/map/'), addEventListener() {}, clearTimeout() {},
        setTimeout(callback) { scheduled = callback; return 1; }},
      Plotly: Object.fromEntries(['react', 'update', 'relayout', 'restyle'].map(method =>
        [method, async (...args) => { calls.push({method, args: clone(args)}); }])),
      startRadarPlot: (_url, render) => { update = render; return {stop() {}}; }
    });
    const source = fs.readFileSync(path.join(__dirname, '../../html/js', `plot_${kind}.js`), 'utf8');
    assert.ok(!source.includes('setInterval'), `${kind} must use shared frame timing`);
    vm.runInContext(source, context);
    assert.equal(typeof update, 'function');
    if (kind === 'map') {
      const mobile = context.delayDopplerPresentation(390);
      const desktop = context.delayDopplerPresentation(1200);
      assert.ok(mobile.margin.l + mobile.margin.r < desktop.margin.l + desktop.margin.r,
        'Phone layout must reserve materially more width for the delay-Doppler heatmap');
      assert.equal(mobile.xTitle, 'Range (km)');
      assert.equal(mobile.yTitle, 'Hz');
      assert.equal(mobile.colorbar.title.text, 'dB');
      assert.ok(mobile.colorbar.thickness < desktop.colorbar.thickness);
      assert.equal(mobile.showScale, false,
        'Phone layout must reserve the narrow viewport for radar data, not a color scale');
      assert.equal(desktop.showScale, true);
      assert.equal(context.mapPresentationUrl('/api/map', 768),
        '/api/map?max_rows=128&max_cols=128',
        'Tablets request the bounded peak-preserving map representation');
      assert.equal(context.mapPresentationUrl('/api/map', 1280), '/api/map',
        'Desktop keeps the full-resolution map');
    }
    if (kind === 'spectrum') {
      const fallback = context.spectrumColorRange([[NaN, Infinity]]);
      assert.equal(fallback.zmin, -120);
      assert.equal(fallback.zmax, 0);
    }
    const frame = {
      map: {timestamp: 1000, nRows: 3, delay: [1], doppler: [1, 2, 3], data: [[1], [2], [3]], maxPower: 15},
      spectrum: {timestamp: [1000], frequency: [[100000, 100001]], spectrum: [[-92, -37]]},
      timing: {timestamp: [1000], cpi: [20], frameTimestamp: 1000},
      detection: {timestamp: [1000], delay: [1], doppler: [2], snr: [3]}
    }[kind];
    assert.notEqual(await update(clone(frame)), false);
    assert.equal(calls.filter(call => call.method === 'react').length, 2, `${kind}: initial real frame must create correct traces without tearing down its plot root`);
    if (kind === 'map') {
      const initialTraces = calls.filter(call => call.method === 'react').at(-1).args[1];
      assert.equal(initialTraces[2].marker.symbol, 'circle',
        'ADS-B truth stays a uniform green-dot overlay on delay–Doppler');
      assert.equal(initialTraces[1].marker.symbol, 'circle',
        'Radar detections keep their established circular marker styling');
      assert.equal(initialTraces[1].marker.size, 8, 'Raw detections are small, light markers');
      assert.equal(initialTraces[3].marker.color, '#ff7a45',
        'Updated radar tracks render as strongly outlined orange markers');
      assert.equal(initialTraces[3].marker.symbol, 'circle-open-dot');
      assert.equal(initialTraces[3].x[0], 8 * 299792458 / 2400000 / 1000,
        'Tracker sample bins convert to excess-path kilometres');
      assert.equal(initialTraces[4].marker.symbol, 'circle-open',
        'Coasting persistent tracks remain visible as hollow orange rings');
      assert.equal(initialTraces[4].x[0], 10 * 299792458 / 2400000 / 1000);
      assert.ok(fetchUrls.some(url => url.includes('/api/detection') && url.includes('frame_timestamp=1000')));
      assert.ok(fetchUrls.some(url => url.includes('/api/tracker') && url.includes('frame_timestamp=1000')),
        'Both overlays are requested for the exact heatmap frame');
    }
    if (kind === 'spectrum') {
      const trace = calls.at(-1).args[1][0];
      assert.deepEqual(trace.x, [100, 100.001]);
      assert.equal(trace.zauto, false);
      assert.equal(trace.zmin, -92);
      assert.equal(trace.zmax, -37);
      assert.match(trace.colorbar.title.text, /dB/);
    }
    if (kind === 'timing') assert.equal(calls.at(-1).args[1].length, 1, 'Frame metadata is not a processing stage');
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
      assert.equal(latestMapUpdate.args[1].x[3][0], 8 * 299792458 / 2400000 / 1000,
        'The matching updated track is rendered in the same Plotly update');
      assert.equal(latestMapUpdate.args[1].x[4][0], 10 * 299792458 / 2400000 / 1000,
        'A coasting track does not disappear between direct detections');
      assert.ok(fetchUrls.some(url => url.includes('frame_timestamp=1001')));
      const changedGrid = {...clone(frame), timestamp: 1002, delay: [1, 2], data: [[1, 2], [3, 4]]};
      const changedGridCallsBefore = calls.length;
      await update(changedGrid);
      assert.ok(calls.slice(changedGridCallsBefore).some(call => call.method === 'react'),
        'Map grid changes reconcile the existing Plotly root');
      assert.ok(!calls.some(call => call.method === 'newPlot'), 'Map never tears down the plot root');
    }
  }
  console.log('Plot first-frame/update, spectrum dB scale, frequency axis and late-stream tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
