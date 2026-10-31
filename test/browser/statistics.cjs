'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');

const root = path.resolve(__dirname, '../../html');
const period = (eligible, detected, tracked) => ({
  out_of_config_passes: 1, eligible_passes: eligible,
  eligible_no_radar_return: eligible - detected, detected_not_tracked: detected - tracked,
  detected_passes: detected, tracked_passes: tracked,
  detection_yield: detected / eligible, tracking_yield: tracked / eligible,
  out_of_span: {below_detection_floor: 1, above_delay_ceiling: 0,
    outside_doppler_span: 2, stale_truth: 1, missing_exact_timing: 3,
    beyond_clutter_cancellation: 2}
});
const bands = {
  '0–30': {eligible_passes: 4, detected_passes: 3, tracked_passes: 2, tracking_yield: .5},
  '30–60': {eligible_passes: 5, detected_passes: 4, tracked_passes: 3, tracking_yield: .6}
};
const scorecard = {
  decision_grade: false, updated_at: 1700000000000, warning: 'receiver timing unavailable',
  association_lag_cpis: 2, association_lag_ms: 400, evaluation_contract_hash: 'abc123def456789',
  limits: {delayMinKm: 1, delayMaxKm: 100, dopplerMinHz: -800, dopplerMaxHz: 800,
    clutterMaxKm: 5, matchDelayKm: .5, matchDopplerHz: 5},
  data_quality: {exact_timed_cpi_frames: 0, unique_cpi_frames: 100,
    nuisance_accounting: 'excluded_from_denominator'},
  windows: {hour: {...period(10, 8, 6), delay_strata: {bands},
      receiver_slant_range_strata: {bands}, elevation_strata: {bands}, azimuth_strata: {bands},
      azimuth_elevation_strata: {bands: {'0–30|30–60': {azimuth_band: '0–30',
        elevation_band: '30–60', eligible_passes: 4, detection_yield: .75, tracking_yield: .5}}}},
    day: period(20, 15, 12), week: period(30, 23, 18), lifetime: period(40, 32, 24)},
  mix_adjusted_previous_hour_comparison: {current: {detection_yield: .8, tracking_yield: .6},
    baseline: {detection_yield: .7, tracking_yield: .5}, common_support_coverage: .9,
    common_strata: ['a', 'b'], decision_grade: false},
  active_passes: [{flight: 'TEST1', status: 'eligible', eligible_cpis: 5, detected_cpis: 4,
    tracked_cpis: 3, last_truth: {delay_km: 12.3, doppler_hz: 45}}],
  recent_finalized_passes: [{flight: 'TEST2', outcome: 'tracked', eligible_cpis: 8,
    detected_cpis: 7, tracked_cpis: 6, finalized_at: 1700000000000}]
};
const fixtures = {
  '/api/live-scorecard': {data_quality: {exact_timed_cpi_frames: 0, event_time_rejections: 100}},
  '/api/live-scorecard/provisional': scorecard,
  '/api/system/status': {serverId: 'statistics-test', radar: 'receiving', adsbEnabled: true},
  '/api/runtime/config': {process: {data: {cpi: 60}}},
  '/capture/status': {supported: false, available: false, recording: false}
};

const server = http.createServer((request, response) => {
  const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
  if (Object.hasOwn(fixtures, pathname)) {
    response.setHeader('Content-Type', 'application/json');
    response.end(JSON.stringify(fixtures[pathname]));
    return;
  }
  const filename = path.resolve(root, '.' + (pathname.endsWith('/') ? pathname + 'index.html' : pathname));
  if (!filename.startsWith(root + path.sep) || !fs.existsSync(filename) || !fs.statSync(filename).isFile()) {
    response.writeHead(404); response.end(); return;
  }
  response.setHeader('Content-Type', {'.js': 'application/javascript', '.css': 'text/css',
    '.html': 'text/html', '.svg': 'image/svg+xml'}[path.extname(filename)] || 'application/octet-stream');
  fs.createReadStream(filename).pipe(response);
});

(async () => {
  let browser;
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    browser = await chromium.launch({headless: true,
      ...(process.env.VECTORWARP_CHROME_EXECUTABLE ?
        {executablePath: process.env.VECTORWARP_CHROME_EXECUTABLE} : {})});
    const page = await browser.newPage({viewport: {width: 1280, height: 900}});
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(`http://127.0.0.1:${server.address().port}/display/statistics/`,
      {waitUntil: 'domcontentloaded'});
    await page.waitForSelector('#view-root[data-ready="true"]');
    assert.deepEqual(errors, []);
    assert.deepEqual(await page.locator('.statistics-section > header > h2').allTextContents(),
      ['Performance', 'Current-hour coverage', 'Aircraft evidence']);
    assert.equal(await page.getByText('Provisional timing', {exact: false}).count(), 0);
    assert.equal(await page.getByText('Exact-timed CPI frames', {exact: false}).count(), 0);
    assert.equal(await page.locator('.metric-card').count(), 4);
    assert.equal(await page.locator('.statistics-details').getAttribute('open'), null);
    assert.match(await page.locator('.statistics-score').innerText(), /Lifetime/);
    assert.match(await page.getByRole('heading', {name: 'Angle bands'}).locator('..').innerText(),
      /Elevation deg/);

    await page.setViewportSize({width: 390, height: 844});
    const layout = await page.evaluate(() => ({
      viewport: document.documentElement.clientWidth,
      bodyWidth: document.body.scrollWidth,
      columns: getComputedStyle(document.querySelector('.insight-stats')).gridTemplateColumns,
      sectionWidth: document.querySelector('.statistics-section').getBoundingClientRect().width
    }));
    assert.equal(layout.bodyWidth, layout.viewport, 'The statistics page must not overflow the mobile viewport');
    assert.equal(layout.columns.trim().split(/\s+/).length, 2, 'Summary cards should remain a compact 2 × 2 grid');
    assert.ok(layout.sectionWidth <= layout.viewport);
    await page.locator('.statistics-details > summary').click();
    assert.match(await page.getByRole('heading', {name: 'Scoring method'}).locator('..').innerText(),
      /Operational and descriptive/);
    console.log('Statistics DOM: hierarchy, timing de-emphasis, scientific detail and mobile layout passed.');
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
