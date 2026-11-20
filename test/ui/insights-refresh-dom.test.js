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
  window: dom.window, document: dom.window.document, URL, URLSearchParams, console,
  Date: class extends Date { static now() { return now; } },
  Plotly: {react: async () => {}},
  sessionStorage: {getItem: () => null, setItem: () => {}},
  addFullscreenControl: () => {}, addRecordingControl: () => {},
  liveApiUrl: path => path, getRadarRuntimeConfig: async () => config,
  getIlluminatorContext: async () => ({enabled: false, id: null}),
  fetchCachedJsonResource: async url => (await context.fetchStatusResource(url)).text().then(JSON.parse),
  radarPresentationUrl: path => `${path}${path.includes('?') ? '&' : '?'}presentation=aligned`,
  startRadarUpdates: callback => { refresh = callback; return {stop() {}}; },
  fetchStatusResource: async url => {
    if (failRequests) throw new Error('Request timed out');
    return {ok: true, status: 200, text: async () =>
      JSON.stringify(url.startsWith('/api/detection') ? detection : config)};
  }
});
require('./display-fixture')(context);
vm.runInContext(fs.readFileSync(require.resolve('../../html/js/insights.js'), 'utf8'), context,
  {filename: 'insights.js'});

(async () => {
  failRequests = true;
  await refresh();
  const root = dom.window.document.getElementById('view-root');
  assert.match(root.textContent, /cannot reach its live data/);
  assert.equal(root.getAttribute('aria-busy'), 'false',
    'Initial-load failure releases the busy state so its error is accessible');
  assert.equal(root.dataset.loading, 'false');
  failRequests = false;
  now += 2000;
  await refresh();
  const table = root.querySelector('.data-table-wrap');
  assert.ok(table, 'initial detection table rendered');
  const firstRow = table.querySelector('tbody tr');
  const firstCell = firstRow.firstElementChild;
  let tableClicks = 0;
  table.addEventListener('click', () => tableClicks++);
  table.focus();
  detection = {timestamp: 2, delay: [4, 7], doppler: [5, 8], snr: [9, 6]};
  now += 2000;
  await refresh();
  assert.strictEqual(root.querySelector('.data-table-wrap'), table,
    'periodic refresh preserves the scrollable table node and its focus state');
  assert.strictEqual(dom.window.document.activeElement, table,
    'periodic refresh does not reset the focused control');
  assert.match(root.textContent, /4\.00/, 'fresh data updates in place');
  assert.equal(table.querySelectorAll('tbody tr').length, 2,
    'new detections append rows inside the existing table');
  assert.strictEqual(table.querySelector('tbody tr'), firstRow);
  assert.strictEqual(firstRow.firstElementChild, firstCell);
  table.click();
  assert.equal(tableClicks, 1, 'refresh retains listeners on existing DOM nodes');

  detection = {timestamp: 3, delay: [12], doppler: [13], snr: [14]};
  now += 2000;
  await refresh();
  assert.equal(table.querySelectorAll('tbody tr').length, 1,
    'expired detections remove trailing rows');
  assert.strictEqual(table.querySelector('tbody tr'), firstRow);
  assert.match(firstRow.textContent, /12\.00/);

  detection = {timestamp: 4, delay: [], doppler: [], snr: []};
  now += 2000;
  await refresh();
  const empty = root.querySelector('.view-empty');
  assert.ok(empty, 'an empty frame replaces the table with the empty state');
  assert.equal(empty, table, 'same-tag containers keep their DOM identity');
  assert.equal(empty.hasAttribute('tabindex'), false);
  assert.equal(empty.hasAttribute('role'), false);
  assert.equal(empty.hasAttribute('aria-label'), false);
  assert.equal(empty.firstElementChild.tagName, 'STRONG');
  assert.equal(empty.textContent, 'No detections');

  detection = {timestamp: 5, delay: [15], doppler: [16], snr: [17]};
  now += 2000;
  await refresh();
  assert.strictEqual(root.querySelector('.data-table-wrap'), table);
  assert.equal(table.getAttribute('tabindex'), '0');
  assert.equal(table.getAttribute('role'), 'region');
  assert.equal(table.getAttribute('aria-label'), 'Data table');
  assert.equal(table.firstElementChild.tagName, 'TABLE');
  assert.match(table.textContent, /15\.00/);
  table.click();
  assert.equal(tableClicks, 2, 'listeners survive empty-state and recovery transitions');

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
      JSON.stringify(url.startsWith('/api/detection') ? detection : config)};
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
