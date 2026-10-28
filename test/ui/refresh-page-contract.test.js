'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '../..');
const htmlRoot = path.join(root, 'html');
function indexPages(directory, prefix = '') {
  const pages = [];
  for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
    if (!entry.isDirectory()) continue;
    const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
    const child = path.join(directory, entry.name);
    if (fs.existsSync(path.join(child, 'index.html'))) pages.push(relative);
    pages.push(...indexPages(child, relative));
  }
  return pages;
}
const pages = ['index', ...indexPages(htmlRoot)].sort();

const insightPages = new Set([
  'display/activity', 'display/calibration', 'display/detections',
  'display/evaluation', 'display/health', 'display/locations',
  'display/overview', 'display/site', 'display/statistics', 'display/tracks'
]);
const plotPages = new Map([
  ['index', 'plot_map.js'], ['display/map', 'plot_map.js'],
  ['display/maxhold', 'plot_map.js'], ['display/spectrum', 'plot_spectrum.js'],
  ['display/timing', 'plot_timing.js'],
  ['display/detection/delay', 'plot_detection.js'],
  ['display/detection/doppler', 'plot_detection.js'],
  ['display/detection/delay-doppler', 'plot_detection.js']
]);
const nonLivePages = new Set(['controller', 'display/configuration', 'display/recordings']);

for (const page of pages) {
  const html = fs.readFileSync(page === 'index' ? path.join(htmlRoot, 'index.html') :
    path.join(htmlRoot, page, 'index.html'), 'utf8');
  assert.match(html, /common\.js/, `${page} must use the shared bounded API client`);
  if (insightPages.has(page)) {
    assert.match(html, /insights\.js/, `${page} must use shared snapshot-preserving refresh`);
  } else if (plotPages.has(page)) {
    assert.match(html, new RegExp(plotPages.get(page).replace('.', '\\.')),
      `${page} must use the shared non-destructive plot refresh`);
  } else {
    assert.ok(nonLivePages.has(page),
      `${page} is a new display page without an explicit refresh contract`);
  }
}

assert.deepEqual(pages, [...insightPages, ...plotPages.keys(), ...nonLivePages].sort(),
  'every display page must be covered by a refresh behavior regression');

const insights = fs.readFileSync(path.join(root, 'html/js/insights.js'), 'utf8');
for (const page of insightPages)
  assert.match(insights, new RegExp(`(?:^|[, {])${page.split('/').at(-1)}:\\s*render`, 'm'),
    `${page} must remain registered with the shared refresh controller`);
assert.match(insights, /root\.dataset\.ready\s*=\s*['"]true['"]/,
  'successful insight renders must become durable snapshots');
assert.match(insights, /minimumRefreshIntervalMs\s*=\s*2000/,
  'multi-endpoint insight refreshes must not run at sub-second CPI cadence');

const common = fs.readFileSync(path.join(root, 'html/js/common.js'), 'utf8');
assert.match(common, /capture\/status\?summary=1/,
  'shared recording control must not poll the multi-megabyte clip ledger');

for (const script of new Set(plotPages.values())) {
  const source = fs.readFileSync(path.join(root, 'html/js', script), 'utf8');
  assert.match(source, /startRadarPlot\(/,
    `${script} must preserve the last valid plot on fetch failure`);
}

console.log(`Refresh contract covers all ${pages.length} display pages.`);
