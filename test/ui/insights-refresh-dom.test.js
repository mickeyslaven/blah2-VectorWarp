'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const {JSDOM} = require('./jsdom');

const dom = new JSDOM(`<!doctype html><body data-insight-view="detections">
  <span id="view-state"></span><div id="view-root"></div></body>`,
{url: 'http://radar.example.invalid/display/detections'});
let refresh;
let detection = {timestamp: 1, delay: [1], doppler: [2], snr: [3]};
const config = {process: {data: {cpi: 1}, detection: {enable: true}}};
const context = vm.createContext({
  window: dom.window, document: dom.window.document, URL, console,
  Plotly: {react: async () => {}},
  sessionStorage: {getItem: () => null, setItem: () => {}},
  addFullscreenControl: () => {}, addRecordingControl: () => {},
  liveApiUrl: path => path, getRadarRuntimeConfig: async () => config,
  startRadarUpdates: callback => { refresh = callback; return {stop() {}}; },
  fetchStatusResource: async url => ({ok: true, status: 200, text: async () =>
    JSON.stringify(url === '/api/detection' ? detection : config)})
});
vm.runInContext(fs.readFileSync(require.resolve('../../html/js/insights.js'), 'utf8'), context,
  {filename: 'insights.js'});

(async () => {
  await refresh();
  const root = dom.window.document.getElementById('view-root');
  const table = root.querySelector('.data-table-wrap');
  assert.ok(table, 'initial detection table rendered');
  table.focus();
  detection = {timestamp: 2, delay: [4], doppler: [5], snr: [6]};
  await refresh();
  assert.strictEqual(root.querySelector('.data-table-wrap'), table,
    'periodic refresh preserves the scrollable table node and its focus state');
  assert.strictEqual(dom.window.document.activeElement, table,
    'periodic refresh does not reset the focused control');
  assert.match(root.textContent, /4\.00/, 'fresh data updates in place');
  console.log('Insight refresh preserves live DOM state.');
})().catch(error => { console.error(error); process.exitCode = 1; });
