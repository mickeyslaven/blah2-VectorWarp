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
const server = http.createServer((request, response) => {
  const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
  if (Object.hasOwn(fixtures, pathname)) {
    response.setHeader('Content-Type', 'application/json');
    response.end(JSON.stringify(fixtures[pathname])); return;
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
    stage = 'map style and dimensions after viewport resize';
    for (const width of [1200, 1050, 1400]) await page.setViewportSize({width, height: 900});
    await page.waitForFunction(() => {
      const graph = document.getElementById('data');
      // Autosizing leaves layout.width unset; _fullLayout holds rendered dimensions.
      return graph?._fullLayout?.mapbox?._subplot?.map?.isStyleLoaded() &&
        graph._fullLayout.width === Math.floor(graph.getBoundingClientRect().width);
    });
    stage = 'live refresh and final assertions';
    await page.waitForTimeout(1200); // More than one live refresh after the resize.
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
    assert.equal(state.center.lat, 40.05);
    assert.equal(state.center.lon, -80.1);
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
