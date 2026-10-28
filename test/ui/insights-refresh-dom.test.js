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
let failRequests = false;
let now = 10000;
const config = {process: {data: {cpi: 1}, detection: {enable: true}}};
const context = vm.createContext({
  window: dom.window, document: dom.window.document, URL, console,
  Date: class extends Date { static now() { return now; } },
  Plotly: {react: async () => {}},
  sessionStorage: {getItem: () => null, setItem: () => {}},
  addFullscreenControl: () => {}, addRecordingControl: () => {},
  liveApiUrl: path => path, getRadarRuntimeConfig: async () => config,
  startRadarUpdates: callback => { refresh = callback; return {stop() {}}; },
  fetchStatusResource: async url => {
    if (failRequests) throw new Error('Request timed out');
    return {ok: true, status: 200, text: async () =>
      JSON.stringify(url === '/api/detection' ? detection : config)};
  }
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
  now += 2000;
  await refresh();
  assert.strictEqual(root.querySelector('.data-table-wrap'), table,
    'periodic refresh preserves the scrollable table node and its focus state');
  assert.strictEqual(dom.window.document.activeElement, table,
    'periodic refresh does not reset the focused control');
  assert.match(root.textContent, /4\.00/, 'fresh data updates in place');

  const snapshot = root.innerHTML;
  const goodState = dom.window.document.getElementById('view-state').textContent;
  failRequests = true;
  for (let attempt = 0; attempt < 2; attempt++) {
    now += 2000;
    await refresh();
  }
  assert.equal(root.innerHTML, snapshot,
    'transient refresh timeouts preserve the last valid page');
  assert.equal(dom.window.document.getElementById('view-state').textContent, goodState,
    'one or two transient timeouts do not flash an unavailable state');
  now += 2000;
  await refresh();
  assert.equal(root.innerHTML, snapshot,
    'repeated timeouts still preserve the last valid page');
  assert.match(dom.window.document.getElementById('view-state').textContent,
    /Refresh delayed.*showing last update/,
    'persistent failure is honest about showing stale data');

  let requests = 0;
  failRequests = false;
  context.fetchStatusResource = async url => {
    requests++;
    return {ok: true, status: 200, text: async () =>
      JSON.stringify(url === '/api/detection' ? detection : config)};
  };
  await refresh();
  assert.equal(requests, 0, 'insight refreshes are throttled between two-second polls');
  now += 2000;
  await refresh();
  assert.ok(requests > 0, 'the next scheduled refresh retries automatically');
  assert.doesNotMatch(dom.window.document.getElementById('view-state').textContent,
    /unavailable|delayed/i, 'a successful retry clears the stale-data warning');
  console.log('Insight refresh preserves live state, throttles requests, and recovers after timeouts.');
})().catch(error => { console.error(error); process.exitCode = 1; });
