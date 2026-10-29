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
    assert.ok(bounds.y < 180, 'mobile fullscreen control belongs in the top-right plot corner');
    const visualBounds = await page.locator('.radar-panel .visualization').boundingBox();
    assert.ok(visualBounds && visualBounds.height <= 500 && visualBounds.height / visualBounds.width < 1.4,
      `portrait delay-Doppler must remain a readable overview, got ${JSON.stringify(visualBounds)}`);
    const legendBounds = await page.locator('.radar-overlay-key').boundingBox();
    assert.ok(legendBounds && legendBounds.x + legendBounds.width <= bounds.x - 2,
      'mobile marker key must not run underneath the fullscreen control');
    const markerBounds = await page.locator('.radar-overlay-control').boundingBox();
    const recordingBounds = await page.locator('.recording-control').boundingBox();
    assert.ok(markerBounds && recordingBounds && markerBounds.y + markerBounds.height <= recordingBounds.y,
      'mobile marker selector must stay above the recording controls');
    await button.click();
    await page.getByRole('button', {name: 'Exit fullscreen'}).waitFor();
    bounds = await page.getByRole('button', {name: 'Exit fullscreen'}).boundingBox();
    assert.ok(bounds && bounds.width <= 40 && bounds.height <= 40,
      `fallback fullscreen control must stay compact, got ${JSON.stringify(bounds)}`);
    assert.equal(await page.locator('.radar-panel.fullscreen-fallback').count(), 1);
    console.log('Mobile radar fullscreen control stays compact before and after fallback fullscreen.');
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
