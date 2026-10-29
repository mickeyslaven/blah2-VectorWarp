'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '../..');
const read = relative => fs.readFileSync(path.join(root, relative), 'utf8');
const pageFiles = [];
function collect(directory) {
  for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
    const file = path.join(directory, entry.name);
    if (entry.isDirectory()) collect(file);
    else if (entry.name === 'index.html') pageFiles.push(file);
  }
}
collect(path.join(root, 'html'));

assert.equal(pageFiles.length, 21, 'Every website route is included in the audit contract');
for (const file of pageFiles) {
  const html = fs.readFileSync(file, 'utf8');
  const route = path.relative(root, file);
  assert.match(html, /<h1(?:\s|>)/i, `${route} must expose a page heading`);
  if (route === 'html/display/map/index.html' || route === 'html/display/evaluation/index.html') {
    assert.match(html, /location\.replace\(/,
      `${route} must redirect to its canonical replacement`);
    continue;
  }
  assert.match(html, /common\.js/, `${route} must use shared navigation/status behavior`);
}

const capturePages = pageFiles.filter(file =>
  /data-capture-controls="true"/.test(fs.readFileSync(file, 'utf8')));
assert.deepEqual(capturePages.map(file => path.relative(root, file)), ['html/index.html'],
  'Only the canonical Live radar route may start or stop raw-IQ capture');

const common = read('html/js/common.js');
assert.match(common, /dataset\.captureControls !== 'true'/,
  'Shared chart scaffolding must not add recording controls without an explicit opt-in');
assert.match(common, /aria-current="page"/,
  'Primary navigation exposes the current route to assistive technology');
assert.match(common, /consecutiveFailures >= 3/,
  'Plot pages expose repeated refresh failures without erasing the last valid frame');
assert.match(common, /Waiting for the first radar frame/,
  'Plot pages distinguish a valid empty stream from loading');

const css = read('html/lib/blah2.css');
assert.match(css, /\.page-intro \{[^}]*clip-path:inset\(50%\)/,
  'Plot headings remain available to screen readers');
assert.match(css, /@media \(max-width:760px\)[\s\S]*\.live-status \{ display:flex;/,
  'Mobile retains operational status instead of hiding it');
assert.match(css, /grid-template-columns:repeat\(5,minmax\(0,1fr\)\)/,
  'The five primary navigation items receive five mobile grid columns');
assert.match(css, /\.setup-wizard-shell \{[^}]*grid-template-columns:minmax\(0,1fr\)/,
  'The mobile setup wizard cannot expand beyond the phone viewport');
assert.match(css, /\.setup-wizard nav \{[^}]*grid-template-columns:repeat\(3,minmax\(0,1fr\)\)/,
  'All six setup steps remain visibly selectable in two mobile rows');

const recordings = read('html/js/recordings.js');
assert.doesNotMatch(recordings, /api\/config\?restart=true/,
  'Recordings must not bypass Settings restart/conflict recovery');
assert.match(recordings, /display\/configuration\?replayFile=/,
  'Replay routes through Settings for review');

const assistant = read('html/js/setup_assistant.js');
assert.match(assistant, /aria-current="step"/);
assert.match(assistant, /aria-pressed="true"/);
assert.match(assistant, /aria-labelledby.*setup-wizard-title/);

const controller = read('html/controller/index.html');
const statisticsPage = read('html/display/statistics/index.html');
const insights = read('html/js/insights.js');
assert.doesNotMatch(controller, /href="\/display\/evaluation"/,
  'ADS-B evaluation must not remain a duplicate navigation destination');
assert.match(statisticsPage, /data-validation-tab="results"/);
assert.match(statisticsPage, /data-validation-tab="adsb"/,
  'Radar validation keeps scorecards and live ADS-B comparison in one workspace');
const locationsRenderer = insights.slice(insights.indexOf('async function renderLocations()'),
  insights.indexOf('async function renderEvaluation()'));
const calibrationRenderer = insights.slice(insights.indexOf('async function renderCalibration()'),
  insights.indexOf('const renderers ='));
assert.doesNotMatch(locationsRenderer, /api\/bearing\/diagnostic/,
  'The primary Map must not fetch engineering-only bearing diagnostics');
assert.match(calibrationRenderer, /api\/bearing\/diagnostic/,
  'Array calibration owns known-angle and wiring diagnostics');
assert.match(insights, /class="overview-health"/,
  'Overview includes the beginner health checklist');
assert.match(insights, /addSource\(radarCoverageSourceId, \{type: 'geojson', data\}\)/,
  'Radar coverage uses a geographic Mapbox source so zoom cannot tear a screen-space path');
assert.match(insights, /source\.setData\(data\)/,
  'Radar coverage updates its existing map layer without rebuilding the map');
assert.match(insights, /layer\._coverageUsesMapLayer \? '' : coveragePath/,
  'The SVG coverage renderer is retained only as a compatibility fallback');
assert.match(insights, /const associationTracks = tracks;/,
  'Hiding map track cages must not disable radar-to-aircraft association');
assert.match(insights, /Radar track layer/,
  'The map summary distinguishes a disabled track layer from missing tracks');

console.log(`UI audit contract covers all ${pageFiles.length} website routes.`);
