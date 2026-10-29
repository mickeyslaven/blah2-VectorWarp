'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');

const root = path.resolve(__dirname, '../../html');
const server = http.createServer((request, response) => {
  const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
  if (pathname.startsWith('/api/') || pathname.startsWith('/capture/')) {
    response.writeHead(404, {'Content-Type': 'application/json'});
    response.end('{}'); return;
  }
  const filename = path.resolve(root, '.' + (pathname === '/' ? '/index.html' : pathname));
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
    const page = await browser.newPage({viewport: {width: 390, height: 844}});
    await page.addInitScript(() => {
      Object.defineProperty(Element.prototype, 'requestFullscreen', {value: undefined, configurable: true});
      Object.defineProperty(Element.prototype, 'webkitRequestFullscreen', {value: undefined, configurable: true});
    });
    await page.goto(`http://127.0.0.1:${server.address().port}/`, {waitUntil: 'domcontentloaded'});
    const button = page.getByRole('button', {name: 'Fullscreen'});
    await button.waitFor();
    let bounds = await button.boundingBox();
    assert.ok(bounds && bounds.width <= 40 && bounds.height <= 40,
      `mobile fullscreen control must stay compact, got ${JSON.stringify(bounds)}`);
    const toolbarBounds = await page.locator('.radar-toolbar').boundingBox();
    assert.ok(toolbarBounds && bounds.y >= toolbarBounds.y &&
      bounds.y + bounds.height <= toolbarBounds.y + toolbarBounds.height,
      'mobile fullscreen control must stay inside the reserved toolbar');
    const visualBounds = await page.locator('.radar-panel .visualization').boundingBox();
    assert.ok(visualBounds && visualBounds.height <= 500 && visualBounds.height / visualBounds.width < 1.4,
      `portrait delay-Doppler must remain a readable overview, got ${JSON.stringify(visualBounds)}`);
    const legendBounds = await page.locator('.radar-overlay-key').boundingBox();
    assert.ok(legendBounds && legendBounds.x + legendBounds.width <= bounds.x - 2,
      'mobile marker key must not run underneath the fullscreen control');
    const markerBounds = await page.locator('.radar-overlay-control').boundingBox();
    const recordingBounds = await page.locator('.recording-control').boundingBox();
    assert.ok(markerBounds && toolbarBounds && markerBounds.y >= toolbarBounds.y &&
      markerBounds.y + markerBounds.height <= toolbarBounds.y + toolbarBounds.height,
      'mobile marker selector must stay inside the reserved toolbar');
    assert.ok(toolbarBounds && visualBounds && toolbarBounds.y + toolbarBounds.height <= visualBounds.y,
      'reserved toolbar must not overlap the delay-Doppler plot');
    assert.ok(visualBounds && recordingBounds && visualBounds.y + visualBounds.height <= recordingBounds.y,
      'delay-Doppler plot must stay above recording controls');
    await button.click();
    await page.getByRole('button', {name: 'Exit fullscreen'}).waitFor();
    bounds = await page.getByRole('button', {name: 'Exit fullscreen'}).boundingBox();
    assert.ok(bounds && bounds.width <= 40 && bounds.height <= 40,
      `fallback fullscreen control must stay compact, got ${JSON.stringify(bounds)}`);
    assert.equal(await page.locator('.radar-panel.fullscreen-fallback').count(), 1);
    await page.setViewportSize({width: 1194, height: 834});
    await page.getByRole('button', {name: 'Exit fullscreen'}).click();
    await page.waitForTimeout(50);
    const tabletToolbar = await page.locator('.radar-toolbar').boundingBox();
    const tabletPlot = await page.locator('.radar-panel .visualization').boundingBox();
    const tabletButton = await page.getByRole('button', {name: 'Fullscreen'}).boundingBox();
    const tabletSelector = await page.locator('.radar-overlay-control').boundingBox();
    assert.ok(tabletToolbar && tabletPlot && tabletToolbar.y + tabletToolbar.height <= tabletPlot.y,
      'iPad landscape toolbar must reserve space above the plot');
    for (const [name, box] of [['fullscreen', tabletButton], ['selector', tabletSelector]]) {
      assert.ok(box && tabletToolbar && box.y >= tabletToolbar.y &&
        box.y + box.height <= tabletToolbar.y + tabletToolbar.height,
        `iPad landscape ${name} control must stay inside the toolbar`);
    }
    const horizontalOverflow = await page.evaluate(() =>
      Math.max(document.documentElement.scrollWidth, document.body.scrollWidth) - innerWidth);
    assert.ok(horizontalOverflow <= 2, `iPad landscape must not overflow by ${horizontalOverflow}px`);
    console.log('Radar controls reserve non-overlapping plot space on phone and iPad landscape.');
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
