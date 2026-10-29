'use strict';
// Real Plotly/Mapbox in Chromium, with synthetic sites/API data and local tiles.
// No receiver, production service, aircraft feed, or live RF is used.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '../../html');
const config = {capture: {fs: 2400000, fc: 527000000, device: {type: 'Kraken', channel_count: 5}},
  process: {data: {cpi: .2}, ambiguity: {delayMax: 245}, tracker: {enable: true}},
  location: {rx: {name: 'Receiver <test>', latitude: 40, longitude: -80, altitude: 250},
    tx: {name: 'Illuminator', latitude: 40.1, longitude: -80.2, altitude: 500}},
  truth: {adsb: {enabled: false}}};
const fixtures = {'/api/runtime/config': config, '/api/config': config,
  '/api/system/status': {serverId: 'synthetic-map-test', radar: 'no-data', adsbEnabled: false},
  '/api/detection': {timestamp: 1, delay: [], doppler: [], snr: []},
  '/api/tracker': {timestamp: 1, data: []},
  '/capture/status': {supported: false, available: false, recording: false},
  '/api/adsb/status': {enabled: false, online: false}};
let detectionRequests = 0;
let trackerRequests = 0;
let inducedRefreshTimeout = false;
const server = http.createServer((request, response) => {
  const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
  if (Object.hasOwn(fixtures, pathname)) {
    if (pathname === '/api/detection' && ++detectionRequests === 6) {
      inducedRefreshTimeout = true;
      // Exercise recovery from one real browser-side API timeout. The request
      // is aborted by readResponse; the following refresh must clear the stale
      // view error after the fixture resumes responding normally.
      return;
    }
    response.setHeader('Content-Type', 'application/json');
    const payload = pathname === '/api/tracker' ? {timestamp: ++trackerRequests,
      data: [{id: 7, state: 'ACTIVE', delay: 18 + trackerRequests, doppler: 40}]} : fixtures[pathname];
    response.end(JSON.stringify(payload)); return;
  }
  const filename = path.resolve(root, '.' + (pathname.endsWith('/') ? pathname + 'index.html' : pathname));
  if (!filename.startsWith(root + path.sep) || !fs.existsSync(filename) || !fs.statSync(filename).isFile()) {
    response.writeHead(404); response.end(); return;
  }
  response.setHeader('Content-Type', {'.js': 'application/javascript', '.css': 'text/css', '.html': 'text/html', '.svg': 'image/svg+xml'}[path.extname(filename)] || 'application/octet-stream');
  fs.createReadStream(filename).pipe(response);
});

(async () => {
  let browser;
  let page;
  let stage = 'browser startup';
  const errors = [];
  const consoleMessages = [];
  const failedRequests = [];
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  try {
    browser = await chromium.launch({headless: true,
      ...(process.env.VECTORWARP_CHROME_EXECUTABLE ? {executablePath: process.env.VECTORWARP_CHROME_EXECUTABLE} : {})});
    page = await browser.newPage({viewport: {width: 1000, height: 700}});
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => {
      if (['error', 'warning'].includes(message.type()) && consoleMessages.length < 30)
        consoleMessages.push({type: message.type(), text: message.text()});
    });
    page.on('requestfailed', request => {
      if (failedRequests.length < 30)
        failedRequests.push({url: request.url(), error: request.failure()?.errorText});
    });
    await page.route(/^https:\/\/[ab]\.tile\.openstreetmap\.org\//, async route => {
      await new Promise(resolve => setTimeout(resolve, 100));
      await route.fulfill({contentType: 'image/png', body: Buffer.from(
        'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=', 'base64')});
    });
    stage = 'initial map and labels';
    await page.goto(base + '/display/locations/', {waitUntil: 'domcontentloaded'});
    await page.waitForFunction(() => document.querySelectorAll('.location-site-label').length === 2);
    assert.deepEqual(await page.locator('.location-site-label').allTextContents(), ['Receiver <test>', 'Illuminator']);
    stage = 'persistent map interaction across refresh intervals';
    await page.evaluate(() => {
      window.locationAfterPlotCount = 0;
      document.getElementById('data').on('plotly_afterplot', () => window.locationAfterPlotCount++);
    });
    const mapBounds = await page.locator('#data').boundingBox();
    assert.ok(mapBounds, 'map must have interactive bounds');
    await page.mouse.move(mapBounds.x + mapBounds.width / 2, mapBounds.y + mapBounds.height / 2);
    await page.mouse.down();
    await page.mouse.up();
    // Let any refresh that began before the click finish before measuring the
    // protected interaction interval.
    await page.waitForTimeout(300);
    const plotsWhileEngaged = await page.evaluate(() => window.locationAfterPlotCount);
    await page.mouse.wheel(0, -180);
    await page.waitForTimeout(2300);
    await page.mouse.wheel(0, -180);
    await page.waitForTimeout(200);
    assert.equal(await page.evaluate(() => window.locationAfterPlotCount), plotsWhileEngaged,
      'live refresh must not reconcile the Mapbox scene while the pointer remains on the map');
    await page.mouse.move(2, 2);
    const resumedDeadline = Date.now() + 4000;
    while (await page.evaluate(() => window.locationAfterPlotCount) <= plotsWhileEngaged &&
      Date.now() < resumedDeadline)
      await page.waitForTimeout(100);
    assert.ok(await page.evaluate(() => window.locationAfterPlotCount) > plotsWhileEngaged,
      'map trace reconciliation must resume after the pointer leaves the map');
    stage = 'touch map interaction persists until an outside tap';
    const touchPlots = await page.evaluate(() => window.locationAfterPlotCount);
    await page.dispatchEvent('#data', 'pointerdown', {pointerType: 'touch', pointerId: 2});
    await page.dispatchEvent('#data', 'pointerup', {pointerType: 'touch', pointerId: 2});
    await page.waitForTimeout(2300);
    assert.equal(await page.evaluate(() => window.locationAfterPlotCount), touchPlots,
      'touch map engagement must survive refreshes between successive gestures');
    await page.dispatchEvent('body', 'pointerdown', {pointerType: 'touch', pointerId: 3});
    const touchResumeDeadline = Date.now() + 4000;
    while (await page.evaluate(() => window.locationAfterPlotCount) <= touchPlots &&
      Date.now() < touchResumeDeadline)
      await page.waitForTimeout(100);
    assert.ok(await page.evaluate(() => window.locationAfterPlotCount) > touchPlots,
      'an outside touch must release the persistent map interaction session');
    const preservedCenter = await page.evaluate(() =>
      document.getElementById('data')._fullLayout.mapbox.center);
    stage = 'map style and dimensions after viewport resize';
    for (const width of [1200, 1050, 1400]) await page.setViewportSize({width, height: 900});
    await page.waitForFunction(() => {
      const graph = document.getElementById('data');
      // Autosizing leaves layout.width unset; _fullLayout holds rendered dimensions.
      return graph?._fullLayout?.mapbox?._subplot?.map?.isStyleLoaded() &&
        graph._fullLayout.width === Math.floor(graph.getBoundingClientRect().width);
    });
    stage = 'live refresh and final assertions';
    const refreshDeadline = Date.now() + 8000;
    while (detectionRequests < 7 && Date.now() < refreshDeadline) await page.waitForTimeout(100);
    assert.equal(inducedRefreshTimeout, true, 'The recovery path must be exercised');
    assert.ok(detectionRequests >= 7, 'A successful refresh must follow the induced timeout');
    await page.waitForFunction(() => window.blah2ViewLastError === null);
    const state = await page.evaluate(() => {
      const graph = document.getElementById('data');
      const sites = graph.data.find(trace => trace.name === 'Radar sites');
      return {error: window.blah2ViewLastError || window.blah2PlotResizeError || null,
        center: graph._fullLayout.mapbox.center, latitude: sites.lat, longitude: sites.lon,
        labels: document.querySelectorAll('.location-site-label').length};
    });
    assert.equal(state.error, null);
    assert.deepEqual(errors, []);
    assert.deepEqual(state.latitude, [40, 40.1]);
    assert.deepEqual(state.longitude, [-80, -80.2]);
    assert.ok(Math.abs(state.center.lat - preservedCenter.lat) < 1e-6,
      'live refresh and resize must preserve the operator map latitude');
    assert.ok(Math.abs(state.center.lon - preservedCenter.lon) < 1e-6,
      'live refresh and resize must preserve the operator map longitude');
    assert.equal(state.labels, 2);
    console.log('Real Chromium locations map: initialization, escaped site labels, updates and resize passed (synthetic data).');
  } catch (error) {
    let state;
    try {
      state = await page?.evaluate(() => {
        const graph = document.getElementById('data');
        const map = graph?._fullLayout?.mapbox?._subplot?.map;
        const bounds = graph?.getBoundingClientRect();
        return {
          labels: document.querySelectorAll('.location-site-label').length,
          mapExists: Boolean(map), styleLoaded: map?.isStyleLoaded(), mapLoaded: map?.loaded(),
          layoutWidth: graph?.layout?.width, fullLayoutWidth: graph?._fullLayout?.width,
          layoutHeight: graph?.layout?.height, fullLayoutHeight: graph?._fullLayout?.height,
          measuredWidth: bounds?.width, measuredHeight: bounds?.height,
          viewportWidth: window.innerWidth, pixelRatio: window.devicePixelRatio,
          viewError: window.blah2ViewLastError, frameError: window.blah2FrameError,
          resizeError: window.blah2PlotResizeError
        };
      });
    } catch (diagnosticError) { state = {diagnosticError: diagnosticError.message}; }
    console.error('Locations map failure diagnostics:', JSON.stringify({
      stage, state, errors, consoleMessages, failedRequests
    }, null, 2));
    throw error;
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
