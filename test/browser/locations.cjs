'use strict';
// Real MapLibre terrain map in Chromium, with synthetic sites/API data and local tiles.
// No receiver, production service, aircraft feed, or live RF is used.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '../../html');
const config = {capture: {fs: 2400000, fc: 527000000, device: {type: 'Kraken', channel_count: 5}},
  process: {data: {cpi: .2}, ambiguity: {delayMax: 245}, tracker: {enable: true},
    bearing: {manifold_recorder: {max_delay_error_km: 100, max_doppler_error_hz: 100}}},
  location: {rx: {name: 'Receiver <test>', latitude: 40, longitude: -80, altitude: 250},
    tx: {name: 'Illuminator', latitude: 40.1, longitude: -80.2, altitude: 500}},
  truth: {adsb: {enabled: true, display_range_km: 1000,
    aircraft_photo_provider: 'planespotters'}}};
const fixtures = {'/api/runtime/config': config, '/api/config': config,
  '/api/system/status': {serverId: 'synthetic-map-test', radar: 'no-data', adsbEnabled: false},
  '/api/detection': {timestamp: 1, delay: [], doppler: [], snr: []},
  '/api/tracker': {timestamp: 1, data: []},
  '/api/radar/coverage': {schema: 'vectorwarp-radar-confirmed-coverage-v1', hours: 24,
    bins: 72, supported_bins: 4, supporting_passes: 3, supporting_aircraft: 2,
    points: [
      {bin: 0, latitude: 40.25, longitude: -80.0},
      {bin: 18, latitude: 40.0, longitude: -79.65},
      {bin: 36, latitude: 39.72, longitude: -80.0},
      {bin: 54, latitude: 40.0, longitude: -80.35}
    ]},
  '/api/adsb': {aircraft: [{hex: 'a0b1c2', flight: 'TEST123', lat: 40.04, lon: -80.05,
    alt_baro: 18000, gs: 220, track: 135, geom_rate: 1800, seen_pos: 0}]},
  '/api/adsb/delay-doppler': {A0B1C2: {hex: 'A0B1C2', delay: 20, doppler: 40}},
  '/api/adsb/aircraft-photo/a0b1c2': {available: true, provider: 'Planespotters.net',
    thumbnail: 'https://t.plnspttrs.net/test.jpg', link: 'https://www.planespotters.net/photo/test',
    photographer: 'Test photographer'},
  '/capture/status': {supported: false, available: false, recording: false},
  '/api/adsb/status': {enabled: true, online: true}};
let detectionRequests = 0;
let trackerRequests = 0;
let inducedRefreshTimeout = false;
let photoRequests = 0;
const server = http.createServer((request, response) => {
  const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
  if (Object.hasOwn(fixtures, pathname)) {
    if (pathname.startsWith('/api/adsb/aircraft-photo/')) photoRequests++;
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
  response.setHeader('Content-Type', {'.js': 'application/javascript', '.mjs': 'application/javascript',
    '.css': 'text/css', '.html': 'text/html', '.svg': 'image/svg+xml'}[path.extname(filename)] || 'application/octet-stream');
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
    await page.route(/^https:\/\/tiles\.mapterhorn\.com\//, async route => {
      if (route.request().url().endsWith('tilejson.json')) {
        await route.fulfill({contentType: 'application/json', body: JSON.stringify({tilejson: '3.0.0',
          tiles: ['https://tiles.mapterhorn.com/tiles/{z}/{x}/{y}.png'], minzoom: 0, maxzoom: 12})});
        return;
      }
      await route.fulfill({contentType: 'image/png', body: Buffer.from(
        'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=', 'base64')});
    });
    stage = 'initial map and labels';
    await page.goto(base + '/display/locations/', {waitUntil: 'domcontentloaded'});
    assert.match(await page.locator('meta[name="viewport"]').getAttribute('content'), /user-scalable=no/,
      'the dedicated map route must never convert a pinch into whole-page zoom');
    await page.waitForFunction(() => document.querySelectorAll('.location-site-label').length === 2);
    await page.waitForFunction(() => document.querySelectorAll('.location-aircraft-marker').length === 1);
    assert.deepEqual(await page.locator('.location-site-label').allTextContents(), ['Receiver <test>', 'Illuminator']);
    assert.equal(await page.locator('.location-3d-model-layer[data-renderer="vectorwarp-real-3d-v1"]').count(), 1,
      'aircraft and sites share the true 3D mesh-rendering layer');
    const renderedPixels = await page.locator('.location-3d-model-layer').evaluate(canvas => {
      const pixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
      let visible = 0;
      for (let index = 3; index < pixels.length; index += 4) if (pixels[index]) visible++;
      return {width: canvas.width, height: canvas.height, visible};
    });
    assert.ok(renderedPixels.width > 0 && renderedPixels.height > 0 && renderedPixels.visible > 100,
      'the real 3D layer must render visible mesh faces');
    assert.equal(await page.locator('.location-aircraft-icon').getAttribute('data-flight-path'), 'climbing');
    await page.setViewportSize({width: 390, height: 844});
    const mobileIconSizes = await page.evaluate(() => ({
      aircraft: getComputedStyle(document.querySelector('.location-aircraft-icon')).width,
      site: getComputedStyle(document.querySelector('.location-site-icon')).width
    }));
    assert.deepEqual(mobileIconSizes, {aircraft: '38px', site: '42px'},
      '3D aircraft and site models remain compact touch-safe overlays on mobile');
    assert.equal(await page.locator('.location-3d-model-layer').evaluate(node => getComputedStyle(node).pointerEvents), 'none',
      'mobile 3D models cannot steal map gestures');
    await page.setViewportSize({width: 1000, height: 700});
    assert.equal(await page.locator('#radar-map-overlay-mode').inputValue(), 'all',
      'the map defaults to showing confirmed radar-track ellipses');
    assert.equal(await page.locator('#adsb-map-filter-mode').inputValue(), 'all',
      'the map defaults to showing all ADS-B aircraft');
    assert.equal(await page.locator('#show-radar-coverage-on-map').isChecked(), true,
      'radar-confirmed rolling coverage is visible by default');
    await page.waitForFunction(() => document.getElementById('data')._vectorwarpMap
      ?.getSource('vectorwarp-radar-coverage'));
    const coverageGeometry = await page.evaluate(() => {
      const map = document.getElementById('data')._vectorwarpMap;
      const source = map.getSource('vectorwarp-radar-coverage');
      return {data: source._vectorwarpData, fill: Boolean(map.getLayer('vectorwarp-radar-coverage-fill')),
        line: Boolean(map.getLayer('vectorwarp-radar-coverage-line')),
        renderer: document.getElementById('data').dataset.mapRenderer,
        pitch: map.getPitch(), maxPitch: map.getMaxPitch(), terrain: map.getTerrain()};
    });
    assert.equal(coverageGeometry.data.features.length, 4,
      'unsupported bearing gaps remain four separate geographic radials');
    assert.ok(coverageGeometry.fill && coverageGeometry.line,
      'coverage fill and line are native geographic map layers');
    assert.equal(coverageGeometry.renderer, 'maplibre-terrain');
    assert.ok(coverageGeometry.pitch > 45 && coverageGeometry.maxPitch >= 80,
      'the terrain renderer starts in a readable 3D view and permits steep pitch');
    assert.equal(coverageGeometry.terrain.source, 'vectorwarp-terrain',
      'the basemap is draped over a real raster DEM source');
    assert.equal(await page.locator('.location-radar-coverage-path').getAttribute('d'), '',
      'the screen-space SVG coverage path is disabled when native map layers are available');
    await page.evaluate(() => {
      const map = document.getElementById('data')._vectorwarpMap;
      map.zoomTo(map.getZoom() + 1, {duration: 0});
    });
    await page.waitForTimeout(100);
    assert.equal(await page.evaluate(() => document.getElementById('data')._vectorwarpMap
      .getSource('vectorwarp-radar-coverage')._vectorwarpData.features.length), 4,
    'zooming preserves the geographic coverage geometry without rebuilding screen paths');
    assert.ok((await page.evaluate(() => document.getElementById('data')._vectorwarpMap.getZoom())) > 5,
      'online ADS-B acquisition radius must not define the radar map distance');
    assert.equal(await page.locator('.location-radar-track-path').count(), 1);
    assert.equal(await page.locator('.location-aircraft-tooltip').count(), 0,
      'map markers must remain label-only; aircraft data belongs in the sidebar');
    assert.equal(await page.locator('.location-aircraft-label').textContent(), 'TEST123');
    await page.waitForFunction(() => window.photoRequestsForTest === undefined || true);
    await page.waitForFunction(() => document.querySelector('.location-radar-track-path')?.getAttribute('d'));
    assert.equal(await page.locator('.location-radar-track-cage').count(), 1,
      'each confirmed track gets one mathematically honest bistatic ellipse');
    const firstTrackPath = await page.locator('.location-radar-track-path').getAttribute('d');
    const overlayStyle = await page.evaluate(() => ({
      overflow: getComputedStyle(document.querySelector('.location-aircraft-trails')).overflow,
      coverageFilter: getComputedStyle(document.querySelector('.location-radar-coverage-path')).filter,
      trackFilter: getComputedStyle(document.querySelector('.location-radar-track-path')).filter,
      trailFilter: getComputedStyle(document.querySelector('.location-aircraft-flight-trail')).filter
    }));
    assert.equal(overlayStyle.overflow, 'hidden', 'offscreen SVG geometry must be clipped');
    assert.equal(overlayStyle.coverageFilter, 'none', 'coverage outline cannot use Safari-smearing SVG filters');
    assert.equal(overlayStyle.trackFilter, 'none', 'radar paths cannot use Safari-smearing SVG filters');
    assert.equal(overlayStyle.trailFilter, 'none', 'flight trails cannot use Safari-smearing SVG filters');
    const mapTouchAction = await page.evaluate(() =>
      getComputedStyle(document.querySelector('.location-map-panel .maplibregl-map')).touchAction);
    assert.equal(mapTouchAction, 'none', 'map gestures must not zoom or scroll the surrounding web page');
    await page.evaluate(() => document.getElementById('data')._vectorwarpMap.fire('movestart'));
    assert.notEqual(await page.locator('.location-aircraft-trails').evaluate(node =>
      getComputedStyle(node).visibility), 'hidden',
    'projected paths must remain visible while the operator rotates or zooms the map');
    assert.notEqual(await page.locator('.location-3d-model-layer').evaluate(node =>
      getComputedStyle(node).visibility), 'hidden',
    '3D aircraft and sites must remain visible during map navigation');
    await page.evaluate(() => document.getElementById('data')._vectorwarpMap.fire('moveend'));
    await page.waitForFunction(() => getComputedStyle(document.querySelector('.location-aircraft-trails')).visibility !== 'hidden');
    const fullscreenBounds = await page.getByRole('button', {name: 'Fullscreen'}).boundingBox();
    const sidebarBounds = await page.locator('.location-map-sidebar').boundingBox();
    assert.ok(fullscreenBounds && sidebarBounds &&
      fullscreenBounds.x + fullscreenBounds.width < sidebarBounds.x,
    'fullscreen control must remain outside the map information overlay');
    stage = 'persistent map interaction across refresh intervals';
    const mapIdentity = await page.evaluate(() => {
      const map = document.getElementById('data')._vectorwarpMap;
      map.__vectorwarpTestIdentity = crypto.randomUUID();
      return map.__vectorwarpTestIdentity;
    });
    const mapBounds = await page.locator('#data').boundingBox();
    assert.ok(mapBounds, 'map must have interactive bounds');
    await page.mouse.move(mapBounds.x + mapBounds.width / 2, mapBounds.y + mapBounds.height / 2);
    await page.mouse.down();
    await page.mouse.up();
    await page.waitForTimeout(300);
    await page.mouse.wheel(0, -180);
    await page.waitForTimeout(2300);
    assert.notEqual(await page.locator('.location-radar-track-path').getAttribute('d'), firstTrackPath,
      'radar track overlays must move with the MapLibre camera');
    await page.mouse.wheel(0, -180);
    await page.waitForTimeout(200);
    assert.equal(await page.evaluate(() => document.getElementById('data')._vectorwarpMap
      .__vectorwarpTestIdentity), mapIdentity,
    'live refresh must update map sources without recreating the map');
    await page.mouse.move(2, 2);
    const requestsAfterLeave = trackerRequests;
    const resumedDeadline = Date.now() + 4000;
    while (trackerRequests <= requestsAfterLeave && Date.now() < resumedDeadline)
      await page.waitForTimeout(100);
    assert.ok(trackerRequests > requestsAfterLeave,
      'live data refresh must continue after the pointer leaves the map');
    stage = 'touch map interaction persists until an outside tap';
    await page.dispatchEvent('#data', 'pointerdown', {pointerType: 'touch', pointerId: 2});
    await page.dispatchEvent('#data', 'pointerup', {pointerType: 'touch', pointerId: 2});
    await page.waitForTimeout(2300);
    assert.equal(await page.evaluate(() => document.getElementById('data')._vectorwarpMap
      .__vectorwarpTestIdentity), mapIdentity,
    'touch interaction and live refresh must preserve one map instance');
    await page.dispatchEvent('body', 'pointerdown', {pointerType: 'touch', pointerId: 3});
    const touchRequests = trackerRequests;
    // This may coincide with the fixture's intentional API timeout. Give the
    // normal retry cadence enough time to prove refresh resumes afterward.
    const touchResumeDeadline = Date.now() + 8000;
    while (trackerRequests <= touchRequests && Date.now() < touchResumeDeadline)
      await page.waitForTimeout(100);
    assert.ok(trackerRequests > touchRequests,
      'an outside touch must leave live data refresh active');
    const preservedCenter = await page.evaluate(() => {
      const center = document.getElementById('data')._vectorwarpMap.getCenter();
      return {lat: center.lat, lon: center.lng};
    });
    stage = 'map style and dimensions after viewport resize';
    for (const width of [1200, 1050, 1400]) {
      await page.setViewportSize({width, height: 900});
      await page.evaluate(() => document.getElementById('data')._vectorwarpMap.resize());
    }
    await page.waitForFunction(() => {
      const graph = document.getElementById('data');
      const map = graph?._vectorwarpMap;
      return map?.isStyleLoaded() && map.getCanvas().clientWidth === Math.floor(graph.getBoundingClientRect().width);
    });
    stage = 'live refresh and final assertions';
    const refreshDeadline = Date.now() + 8000;
    while (detectionRequests < 7 && Date.now() < refreshDeadline) await page.waitForTimeout(100);
    assert.equal(inducedRefreshTimeout, true, 'The recovery path must be exercised');
    assert.ok(detectionRequests >= 7, 'A successful refresh must follow the induced timeout');
    await page.waitForFunction(() => window.blah2ViewLastError === null);
    const state = await page.evaluate(() => {
      const graph = document.getElementById('data');
      const center = graph._vectorwarpMap.getCenter();
      return {error: window.blah2ViewLastError || window.blah2PlotResizeError || null,
        center: {lat: center.lat, lon: center.lng}, renderer: graph.dataset.mapRenderer,
        labels: document.querySelectorAll('.location-site-label').length};
    });
    assert.equal(state.error, null);
    assert.deepEqual(errors, []);
    assert.equal(state.renderer, 'maplibre-terrain');
    assert.ok(Math.abs(state.center.lat - preservedCenter.lat) < 1e-6,
      'live refresh and resize must preserve the operator map latitude');
    assert.ok(Math.abs(state.center.lon - preservedCenter.lon) < 1e-6,
      'live refresh and resize must preserve the operator map longitude');
    assert.equal(state.labels, 2);
    assert.equal(photoRequests, 1, 'aircraft photo metadata must be prefetched once and reused from cache');
    stage = 'mobile aircraft label collision avoidance';
    fixtures['/api/adsb'] = {aircraft: Array.from({length: 54}, (_, index) => ({
      hex: (0xa10000 + index).toString(16), flight: `T${String(index).padStart(3, '0')}`,
      lat: 38.7 + (index % 9) * .32, lon: -82.2 + Math.floor(index / 9) * .8,
      alt_baro: 8000 + index * 500, gs: 190, track: 70, seen_pos: 0
    }))};
    await page.setViewportSize({width: 390, height: 844});
    await page.evaluate(async () => {
      const graph = document.getElementById('data');
      const map = graph._vectorwarpMap;
      map.resize();
      map.jumpTo({center: [-80.08, 40.02], zoom: 5.8, bearing: 0, pitch: 0});
    });
    await page.waitForFunction(() => document.querySelectorAll('.location-aircraft-marker').length >= 50,
      null, {timeout: 7000});
    await page.waitForFunction(() => {
      const visible = [...document.querySelectorAll('.location-aircraft-label')]
        .filter(label => getComputedStyle(label).display !== 'none').length;
      return visible > 0 && visible <= 22;
    }, null, {timeout: 7000});
    const labelLayout = await page.evaluate(() => {
      const labels = [...document.querySelectorAll('.location-aircraft-label')]
        .filter(label => getComputedStyle(label).display !== 'none')
        .map(label => label.getBoundingClientRect());
      let collisions = 0;
      for (let left = 0; left < labels.length; left++) for (let right = left + 1; right < labels.length; right++) {
        const a = labels[left], b = labels[right];
        if (!(a.right + 3 <= b.left || a.left >= b.right + 3 ||
            a.bottom + 2 <= b.top || a.top >= b.bottom + 2)) collisions++;
      }
      return {visible: labels.length, collisions};
    });
    assert.ok(labelLayout.visible > 0 && labelLayout.visible <= 22,
      'the mobile regional map keeps a bounded useful set of aircraft labels');
    assert.equal(labelLayout.collisions, 0,
      'visible mobile aircraft labels must not cover one another');
    const siteLabels = await page.locator('.location-site-name').evaluateAll(labels =>
      labels.map(label => label.getBoundingClientRect()));
    assert.ok(siteLabels.length === 2 && (siteLabels[0].right <= siteLabels[1].left ||
      siteLabels[0].left >= siteLabels[1].right || siteLabels[0].bottom <= siteLabels[1].top ||
      siteLabels[0].top >= siteLabels[1].bottom),
    'receiver and transmitter name plates must use non-overlapping opposite anchors');
    console.log('Real Chromium locations map: initialization, escaped site labels, updates and resize passed (synthetic data).');
  } catch (error) {
    let state;
    try {
      state = await page?.evaluate(() => {
        const graph = document.getElementById('data');
        const map = graph?._vectorwarpMap;
        const bounds = graph?.getBoundingClientRect();
        return {
          labels: document.querySelectorAll('.location-site-label').length,
          mapExists: Boolean(map), styleLoaded: map?.isStyleLoaded(), mapLoaded: map?.loaded(),
          renderer: graph?.dataset?.mapRenderer,
          canvasWidth: map?.getCanvas?.()?.clientWidth, canvasHeight: map?.getCanvas?.()?.clientHeight,
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
