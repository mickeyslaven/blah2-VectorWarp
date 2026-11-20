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
let eligiblePasses = 42;
let requestCount = 0;
const scorecard = () => ({
  updated_at: updatedAt,
  decision_grade: true,
  data_quality: {exact_timed_cpi_frames: 1},
  windows: {
    hour: {eligible_passes: 2, detection_yield: .5, tracking_yield: .5},
    day: {}, week: {},
    lifetime: {eligible_passes: 4, detection_yield: .5, tracking_yield: .25}
  },
  current_setup_lifetime: {eligible_passes: eligiblePasses, detection_yield: .75, tracking_yield: .5},
  active_passes: [], recent_finalized_passes: [], limits: {}
});
const context = vm.createContext({
  window: dom.window, document: dom.window.document, URL, URLSearchParams, console,
  Date: class extends Date { static now() { return now; } },
  sessionStorage: {getItem: () => null, setItem: () => {}},
  addFullscreenControl: () => {}, addRecordingControl: () => {},
  liveApiUrl: path => path, getRadarRuntimeConfig: async () => ({}),
  getIlluminatorContext: async () => ({enabled: false, id: null, illuminators: []}),
  getFederatedChannels: async () => null,
  fetchCachedJsonResource: async url => (await context.fetchStatusResource(url)).text().then(JSON.parse),
  radarPresentationUrl: path => path,
  startRadarUpdates: callback => { refresh = callback; return {stop() {}}; },
  fetchStatusResource: async () => {
    requestCount++;
    return {ok: true, status: 200,
      text: async () => JSON.stringify(scorecard())};
  }
});
require('./display-fixture')(context);
vm.runInContext(fs.readFileSync(require.resolve('../../html/js/insights.js'), 'utf8'), context,
  {filename: 'insights.js'});

(async () => {
  await refresh();
  const root = dom.window.document.getElementById('view-root');
  const details = root.querySelector('.statistics-details');
  const table = root.querySelector('.data-table-wrap');
  const displayedPasses = () => [...root.querySelectorAll('.metric-card')]
    .find(card => card.querySelector('span')?.textContent === 'All-time passes')
    ?.querySelector('strong').textContent;
  assert.ok(details && table, 'statistics view rendered interactive content');
  assert.equal(displayedPasses(), '42',
    'statistics headline uses the persistent current-setup lifetime');
  details.open = true;
  table.scrollTop = 37;
  table.focus();
  dom.window.document.documentElement.scrollTop = 211;

  updatedAt = 2000;
  eligiblePasses = 43;
  const requestsAfterInitialRender = requestCount;
  now += 2000;
  await refresh();

  assert.equal(requestCount, requestsAfterInitialRender,
    'statistics kiosk avoids rebuilding the rich page between 30-second polls');

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
  now += 28000;
  await refresh();
  assert.ok(requestCount > requestsAfterInitialRender,
    'statistics kiosk refreshes after the 30-second interval');
  assert.equal(displayedPasses(), '43', 'fresh statistics update while the menu is open');
  assert.strictEqual(root.querySelector('.statistics-details'), details,
    'an actual data refresh retains the expanded menu node');
  assert.equal(details.open, true,
    'an actual data refresh retains the user-selected open state');
  assert.strictEqual(root.querySelector('.data-table-wrap'), table);
  assert.equal(table.scrollTop, 37);
  assert.strictEqual(dom.window.document.activeElement, table);
  assert.equal(dom.window.document.documentElement.scrollTop, 211);

  details.open = false;
  updatedAt = 3000;
  eligiblePasses = 44;
  now += 30000;
  await refresh();
  assert.equal(displayedPasses(), '44', 'fresh statistics update while the menu is closed');
  assert.strictEqual(root.querySelector('.statistics-details'), details);
  assert.equal(details.open, false, 'later data updates also retain a closed menu');
  console.log('Insight refresh preserves menu, node, focus, local scroll and page scroll state.');
})().catch(error => { console.error(error); process.exitCode = 1; });
