'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const {JSDOM} = require('/tmp/vectorwarp-device-cache-pr91/test/ui/jsdom');

const dom = new JSDOM(`<!doctype html><body data-insight-view="health">
  <span id="view-state"></span><div id="view-root"></div></body>`,
{url: 'http://radar.example.invalid/display/diagnostics?view=health'});
let refresh;
const config = {capture: {device: {channel_count: 5}}, process: {data: {cpi: .5},
  detection: {enable: true}, tracker: {enable: true}}, truth: {adsb: {enabled: false}}};
const timing = {timestamp: 1000, frame_age_ms: 2500, presentation_delay_ms: 2000,
  frame_excess_age_ms: 500, capture_backlog_ms: 503, cpi: 425, uptime_s: 3600};
const tracker = {timestamp: 1000, n: 0, data: []};
const context = vm.createContext({
  window: dom.window, document: dom.window.document, URL, URLSearchParams, console,
  Plotly: {react: async () => {}},
  sessionStorage: {getItem: () => null, setItem: () => {}},
  addFullscreenControl: () => {}, addRecordingControl: () => {},
  liveApiUrl: path => path, getRadarRuntimeConfig: async () => config,
  radarPresentationUrl: path => `${path}${path.includes('?') ? '&' : '?'}presentation=aligned`,
  startRadarUpdates: callback => { refresh = callback; return {stop() {}}; },
  fetchStatusResource: async url => ({ok: true, status: 200, text: async () =>
    JSON.stringify(url.startsWith('/api/timing') ? timing :
      url.startsWith('/api/tracker') ? tracker : config)})
});
vm.runInContext(fs.readFileSync('/tmp/vectorwarp-device-cache-pr91/html/js/insights.js', 'utf8'),
  context, {filename: 'insights.js'});

(async () => {
  await refresh();
  const text = dom.window.document.getElementById('view-root').textContent;
  assert.match(text, /Radar frames500 ms cadence · 2\.0 s aligned holdLive/,
    'The fixed presentation hold is described instead of reported as frame lateness');
  assert.match(text, /Capture backlog1\.01 CPI queued503\.0 ms/,
    'The real one-CPI capture queue is shown in milliseconds and CPIs');
  assert.doesNotMatch(text, /Capture backlogUnavailable/);
  assert.match(text, /HealthyOverall state/, 'Normal sub-cadence processing remains healthy');
  assert.doesNotMatch(text, /MonitorOverall state/,
    'Normal 420–455 ms processing within a 500 ms cadence is not an alarm');
  console.log('Health timing distinguishes aligned presentation age and capture backlog.');
})().catch(error => { console.error(error); process.exitCode = 1; });
