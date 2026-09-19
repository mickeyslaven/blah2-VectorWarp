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
      Plotly: Object.fromEntries(['newPlot', 'update', 'relayout', 'restyle'].map(method =>
        [method, async (...args) => { calls.push({method, args: clone(args)}); }])),
      startRadarPlot: (_url, render) => { update = render; return {stop() {}}; }
    });
    const source = fs.readFileSync(path.join(__dirname, '../../html/js', `plot_${kind}.js`), 'utf8');
    assert.ok(!source.includes('setInterval'), `${kind} must use shared frame timing`);
    vm.runInContext(source, context);
    assert.equal(typeof update, 'function');
    const frame = {
      map: {timestamp: 1000, nRows: 3, delay: [1], doppler: [1, 2, 3], data: [[1], [2], [3]], maxPower: 15},
      spectrum: {timestamp: [1000], frequency: [[100000, 100001]], spectrum: [[1, 2]]},
      timing: {timestamp: [1000], cpi: [20], frameTimestamp: 1000},
      detection: {timestamp: [1000], delay: [1], doppler: [2], snr: [3]}
    }[kind];
    assert.notEqual(await update(clone(frame)), false);
    assert.equal(calls.filter(call => call.method === 'newPlot').length, 2, `${kind}: initial real frame must create correct traces`);
    if (kind === 'spectrum') assert.deepEqual(calls.at(-1).args[1][0].x, [100, 100.001]);
    if (kind === 'timing') assert.equal(calls.at(-1).args[1].length, 1, 'Frame metadata is not a processing stage');
    await update(clone(frame));
    assert.equal(calls.at(-1).method, 'update');
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
    }
  }
  console.log('Original plot first-frame/update, frequency axis and late-stream tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
