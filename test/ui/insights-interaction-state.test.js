'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const {JSDOM} = require('./jsdom');

const dom = new JSDOM(`<!doctype html><body data-insight-view="statistics">
  <span id="view-state"></span><div id="view-root"></div></body>`,
{url: 'http://radar.example.invalid/display/statistics'});
let refresh;
let now = 10000;
let updatedAt = 1000;
const scorecard = () => ({
  updated_at: updatedAt,
  decision_grade: true,
  data_quality: {exact_timed_cpi_frames: 1},
  windows: {
    hour: {eligible_passes: 2, detection_yield: .5, tracking_yield: .5},
    day: {}, week: {},
    lifetime: {eligible_passes: 4, detection_yield: .5, tracking_yield: .25}
  },
  current_setup_lifetime: {eligible_passes: 42, detection_yield: .75, tracking_yield: .5},
  active_passes: [], recent_finalized_passes: [], limits: {}
});
const context = vm.createContext({
  window: dom.window, document: dom.window.document, URL, URLSearchParams, console,
  Date: class extends Date { static now() { return now; } },
  sessionStorage: {getItem: () => null, setItem: () => {}},
  addFullscreenControl: () => {}, addRecordingControl: () => {},
  liveApiUrl: path => path, getRadarRuntimeConfig: async () => ({}),
  radarPresentationUrl: path => path,
  startRadarUpdates: callback => { refresh = callback; return {stop() {}}; },
  fetchStatusResource: async () => ({ok: true, status: 200,
    text: async () => JSON.stringify(scorecard())})
});
vm.runInContext(fs.readFileSync(require.resolve('../../html/js/insights.js'), 'utf8'), context,
  {filename: 'insights.js'});

(async () => {
  await refresh();
  const root = dom.window.document.getElementById('view-root');
  const details = root.querySelector('.statistics-details');
  const table = root.querySelector('.data-table-wrap');
  assert.ok(details && table, 'statistics view rendered interactive content');
  assert.match(root.textContent, /42/,
    'statistics headline uses the persistent current-setup lifetime');
  details.open = true;
  table.scrollTop = 37;
  table.focus();
  dom.window.document.documentElement.scrollTop = 211;

  updatedAt = 2000;
  now += 2000;
  await refresh();

  assert.strictEqual(root.querySelector('.statistics-details'), details,
    'live data refresh preserves the menu node');
  assert.equal(details.open, true,
    'live data refresh preserves the user-selected expanded menu state');
  assert.strictEqual(root.querySelector('.data-table-wrap'), table,
    'live data refresh preserves scrollable data nodes');
  assert.equal(table.scrollTop, 37, 'live data refresh preserves local scroll position');
  assert.strictEqual(dom.window.document.activeElement, table,
    'live data refresh preserves focus');
  assert.equal(dom.window.document.documentElement.scrollTop, 211,
    'live data refresh does not jump the page scroll position');
  console.log('Insight refresh preserves menu, node, focus, local scroll and page scroll state.');
})().catch(error => { console.error(error); process.exitCode = 1; });
