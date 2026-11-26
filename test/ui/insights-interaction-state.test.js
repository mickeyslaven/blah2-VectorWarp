'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const {JSDOM} = require('./jsdom');
const {compactNodeStatistics, combineNodeStatistics} = require('../../api/node-federation');
const {aggregate, compactBasis, kioskAssociationChannel} = require('../../html/js/measured_track_coverage');

const dom = new JSDOM(`<!doctype html><body data-insight-view="statistics">
  <span id="view-state"></span><div id="view-root"></div></body>`,
{url: 'http://radar.example.invalid/display/statistics'});
let refresh;
let now = 10000;
let updatedAt = 1000;
let eligiblePasses = 42;
let requestCount = 0;
let fleet = null, networkSummary = null, remoteSummary = null, federationSummary = null;
const offline = new Set(), stopped = new Set();
let catalogOffline = false;
dom.window.VectorWarpMeasuredCoverage = require('../../html/js/measured_track_coverage');
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
  getFederatedChannels: async () => {if (catalogOffline) throw new Error('Catalog timeout'); return fleet;},
  fetchCachedJsonResource: async url => (await context.fetchStatusResource(url)).text().then(JSON.parse),
  radarPresentationUrl: path => path,
  startRadarUpdates: callback => { refresh = callback; return {stop() {}}; },
  fetchStatusResource: async url => {
    requestCount++;
    const node = url.includes('/api/federation/nodes/hp/') ? 'hp' : 'local';
    if (offline.has(node)) throw new Error('Transport timeout');
    return {ok: true, status: 200,
      text: async () => JSON.stringify(url.includes('/api/federation/statistics-summary') && federationSummary ?
        federationSummary() : networkSummary && url.includes('/api/system/status') ?
        {radar: 'receiving', processorFresh: !stopped.has(node), processor: {state: stopped.has(node) ? 'stopped' : 'live'}} :
          (url.includes('/api/federation/nodes/hp/') ? remoteSummary : null) || networkSummary || scorecard())};
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
  fleet = {nodes: [{id: 'local', label: 'Local', local: true}],
    channels: ['alpha', 'beta'].map(id => ({nodeId: 'local', item: {id}}))};
  const measuredChannel = id => ({selected_channel: id,
    association_statistics: {evaluation_policy_hash: '5'.repeat(64)}, measured_track_basis: {
    version: 1, complete: true, aircraft: [{hex: 'abc123', eligible: [[0, 1000]],
      scored: [[0, 1000]], measured: [[0, 1000]]}]}});
  const measuredFraction = () => root.querySelector('.cpi-kiosk-ring strong')?.textContent;
  for (const ids of [['alpha', 'extra'], ['alpha', 'alpha'], ['primary', 'beta']]) {
    networkSummary = {channels: ids.map(measuredChannel)};
    now += 30000;
    await refresh();
    assert.equal(dom.window.blah2ViewLastError, null);
    assert.equal(measuredFraction(), '—',
      'A missing expected channel cannot claim an exact aggregate');
    assert.equal(root.querySelector('.cpi-kiosk-ring span').textContent, 'Unavailable',
      'Missing evidence is unavailable, not synthetic zero coverage');
    assert.equal(root.querySelector('.cpi-channel-card b').textContent, '— detected',
      'Zero association exposure stays unavailable');
  }
  networkSummary = {channels: ['alpha', 'beta'].map(measuredChannel)};
  for (const [index, counts] of [[10, 8, 4, 2], [15, 12, 9, 6]].entries()) {
    const [eligible, comparable, detected, tracked] = counts;
    networkSummary.channels[index].passes = [{hex: 'abc123', started_at: 0, finalized_at: 1000,
      cpi_exposure: {eligible_cpis: eligible, association_eligible_cpis: comparable,
        detected_cpis: detected, operationally_tracked_cpis: tracked,
        eligible_seconds: eligible / 2, association_eligible_seconds: comparable / 2}}];
  }
  now += 30000;
  await refresh();
  assert.equal(dom.window.blah2ViewLastError, null);
  assert.equal(measuredFraction(), '100.0%', 'Each expected node/channel identity restores complete coverage');
  assert.deepEqual([...root.querySelectorAll('.cpi-channel-card b')].map(node => node.textContent),
    ['50.0% detected', '75.0% detected']);
  assert.deepEqual([...root.querySelectorAll('.cpi-channel-card small')].map(node => node.textContent),
    ['25.0% operationally tracked', '50.0% operationally tracked']);
  assert.deepEqual([...root.querySelectorAll('.cpi-presence-hero strong')].map(node => node.textContent),
    ['12.5 s', '65.0%', '35.0%', '40.0%'],
    'Combined CPI percentages divide summed hits by association exposure, not average channel percentages');
  assert.equal(root.querySelector('.cpi-presence-hero .statistics-hero-tracked small').textContent,
    '60.0% untracked');
  fleet.nodes.push({id: 'hp', label: 'HP', local: false});
  fleet.channels.push({nodeId: 'hp', item: {id: 'primary'}});
  for (const policy of [undefined, '', 'unknown', '4'.repeat(64), '5'.repeat(64)]) {
    remoteSummary = {channels: [measuredChannel('primary')]};
    remoteSummary.channels[0].association_statistics.evaluation_policy_hash = policy;
    // Full contracts legitimately differ across receiver/channel geometry.
    remoteSummary.channels[0].association_statistics.evaluation_contract_hash = 'b'.repeat(64);
    now += 30000;
    await refresh();
    assert.equal(dom.window.blah2ViewLastError, null);
    if (policy === '5'.repeat(64)) {
      assert.equal(measuredFraction(), '100.0%', 'Matching scoring policies restore combined statistics');
      assert.doesNotMatch(root.textContent, /Combined statistics unavailable/);
    } else {
      assert.match(root.textContent, /Combined statistics unavailable/);
      assert.equal(root.querySelector('.cpi-kiosk-ring'), null,
        'Missing, malformed and mixed policies must immediately remove previous combined totals');
    }
  }
  const originalSummary = networkSummary;
  const bridgeChannels = ['alpha','beta'].map(measuredChannel);
  const pass = (start,end,tracked=false) => ({hex:'abc123',started_at:start,
    finalized_at:end,detected:true,tracked});
  bridgeChannels[0].passes=[pass(0,10),pass(20,30)];
  bridgeChannels[1].passes=[pass(5,25,true)];
  for (const channels of [bridgeChannels, [...bridgeChannels].reverse()]) {
    networkSummary={channels}; now+=30000; await refresh();
    assert.equal(root.querySelector('.statistics-hero-eligible strong').textContent,'1',
      'A cross-channel bridge forms one exact pass union regardless of channel order');
    assert.equal(root.querySelector('.statistics-hero-tracked strong').textContent,'100.0%');
  }
  networkSummary=originalSummary;
  fleet.nodes.push({id:'display',label:'Display',role:'coordinator'});
  const serverChannels = [networkSummary.channels, remoteSummary.channels].map(channels =>
    channels.map(channel => ({...channel, measured_track_basis:compactBasis(channel.measured_track_basis),
      measured_track_coverage:aggregate([channel.measured_track_basis])})));
  let coverageMissingBasis = false;
  federationSummary = () => combineNodeStatistics([
    {id:'display',role:'coordinator',available:false,channels:[],passes:[]},
    ...['local','hp'].map((id,index) => compactNodeStatistics({id},
      {radar:'receiving',processorFresh:!offline.has(id)&&!stopped.has(id),processor:{state:'live'}}, {},
      {channels:serverChannels[index].map(channel => coverageMissingBasis && id === 'hp' ?
        {...channel,measured_track_basis:undefined} : channel)}))]);
  networkSummary = {channels:serverChannels[0].map(kioskAssociationChannel)};
  remoteSummary = {channels:serverChannels[1].map(kioskAssociationChannel)};
  assert.ok(networkSummary.channels.every(channel => !('measured_track_basis' in channel)),
    'Real kiosk transport omits raw interval bases');
  dom.window.document.body.classList.add('distance-view');
  dom.reconfigure({url:'http://radar.example.invalid/display/statistics?scope=all&distance_view=1'});
  now += 30000; await refresh();
  const coverageSeconds = () => [...root.querySelectorAll('.cpi-kiosk-gauge.tracked footer strong')]
    .map(node => node.textContent);
  assert.equal(measuredFraction(),'100.0%','Kiosk uses the authoritative exact server union');
  assert.equal(root.querySelector('.cpi-kiosk-ring span').textContent,'Measured',
    'A display-only coordinator does not make radar evidence incomplete');
  assert.deepEqual(coverageSeconds(),['1.0 s','1.0 s'],
    'Overlapping channel time is unioned, never summed or reconstructed as zero');
  coverageMissingBasis=true; now+=30000; await refresh();
  assert.equal(measuredFraction(),'—');
  assert.equal(root.querySelector('.cpi-kiosk-ring span').textContent,'Unavailable');
  assert.deepEqual(coverageSeconds(),['—','—'],'Unavailable exact coverage never renders zero seconds');
  assert.deepEqual([...root.querySelectorAll('.cpi-kiosk-continuity dd')].map(node => node.textContent),
    ['— gaps','—','—'],'Unavailable continuity never renders zero gaps');
  assert.deepEqual([...root.querySelectorAll('.cpi-kiosk-bar.tracked b')].map(node => node.textContent),
    ['100.0%','100.0%','100.0%'],'Per-channel measured summaries survive missing aggregate basis');
  coverageMissingBasis=false; now+=30000; await refresh();
  offline.add('hp'); now += 30000; await refresh();
  assert.match(root.querySelector('#cpi-kiosk-title').textContent, /Cached/,
    'Distance kiosk visibly labels a transport-cached node');
  assert.equal(root.querySelectorAll('.cpi-kiosk-channel').length,3);
  now += 91000; await refresh();
  assert.match(root.querySelector('#cpi-kiosk-title').textContent, /Partial/);
  assert.equal(root.querySelectorAll('.cpi-kiosk-channel').length,2,
    'Expired remote evidence no longer appears in current channel totals');
  offline.add('local'); now += 91000; await refresh();
  assert.equal(root.querySelectorAll('.cpi-kiosk-channel').length,2,
    'All-failed transport refresh preserves the last rendered snapshot');
  assert.match(root.querySelector('#cpi-kiosk-title').textContent, /Cached/);
  assert.doesNotMatch(root.textContent, /Scoring policies do not match/);
  assert.match(dom.window.blah2ViewLastError, /No radar node statistics/);
  offline.clear(); stopped.add('local'); stopped.add('hp'); now += 30000; await refresh();
  assert.match(root.textContent, /No radar nodes are publishing fresh observations/);
  assert.equal(root.querySelector('.cpi-kiosk-ring'),null,
    'Known stopped processors invalidate old totals, unlike a transient transport error');
  stopped.clear(); now += 30000; await refresh();
  catalogOffline=true; now += 30000; await refresh();
  assert.equal(root.querySelectorAll('.cpi-kiosk-channel').length,3);
  assert.match(root.querySelector('#cpi-kiosk-title').textContent,/Cached/);
  catalogOffline=false;
  networkSummary = remoteSummary = {channels: []};
  now += 30000;
  await refresh();
  assert.match(root.textContent, /Combined statistics unavailable/,
    'An empty returned cohort cannot satisfy the single-policy requirement');
  console.log('Insight refresh preserves menu, node, focus, local scroll and page scroll state.');
})().catch(error => { console.error(error); process.exitCode = 1; });
