'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const clone = value => JSON.parse(JSON.stringify(value));

(async () => {
  for (const kind of ['map', 'spectrum', 'timing', 'detection']) {
    const calls = [];
    let update;
    const running = {process: {data: {cpi: .2}, detection: {enable: true}}, truth: {adsb: {enabled: true}}};
    let detected = {timestamp: 1000, delay: [1], doppler: [2]};
    const context = vm.createContext({
      liveApiUrl: url => url,
      urlMap: '/api/map', xTitle: 'Time', yTitle: 'Doppler', xVariable: 'timestamp', yVariable: 'doppler',
      getRadarRuntimeConfig: async () => running,
      fetchRadarJson: async url => url === '/api/detection' ? detected : new Promise(() => {}),
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
    if (kind === 'spectrum') {
      const trace = calls.at(-1).args[1][0];
      assert.deepEqual(trace.x, [100, 100.001]);
      assert.equal(trace.zauto, false);
      assert.equal(trace.zmin, -92);
      assert.equal(trace.zmax, -37);
      assert.match(trace.colorbar.title.text, /dB/);
    }
    if (kind === 'timing') assert.equal(calls.at(-1).args[1].length, 1, 'Frame metadata is not a processing stage');
    await update(clone(frame));
    assert.equal(calls.at(-1).method, 'update');
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
      detected = {...detected, timestamp: 999};
      assert.equal(await update(clone(frame)), false, 'A late detection stream must remain retryable');
      assert.deepEqual(calls.at(-1).args[1].x[1], [], 'Do not overlay an older detection frame');
      const painted = calls.length;
      assert.equal(await update(clone(frame)), false);
      assert.equal(calls.length, painted, 'Waiting for detections must not repaint the heatmap');
      detected = {...detected, timestamp: 1000};
      assert.equal(await update(clone(frame)), true);
      assert.equal(calls.at(-1).method, 'restyle', 'Late detections update only marker trace');
      assert.deepEqual(calls.at(-1).args[2], [1]);
      assert.deepEqual(calls.at(-1).args[1].x, [[1]]);
      const changedGrid = {...clone(frame), timestamp: 1001, delay: [1, 2], data: [[1, 2], [3, 4]]};
      await update(changedGrid);
      assert.ok(calls.slice(-2).some(call => call.method === 'react'),
        'Map grid changes reconcile the existing Plotly root');
      assert.ok(!calls.some(call => call.method === 'newPlot'), 'Map never tears down the plot root');
    }
  }
  console.log('Plot first-frame/update, spectrum dB scale, frequency axis and late-stream tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
