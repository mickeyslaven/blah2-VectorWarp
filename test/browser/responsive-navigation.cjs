'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '../../html');
const config = {capture: {fc: 509e6, fs: 2.4e6, device: {type: 'Kraken', channel_count: 5,
  gain: [1,1,1,1,1], array_geometry: {shape: 'regular_polygon', radius_m: .19}}},
process: {data: {cpi: .5, buffer: 2}, performance: {acceleration: 'gpu-resident'},
  ambiguity: {delayMin: 0, delayMax: 100, dopplerMin: -100, dopplerMax: 100},
  clutter: {enable: true}, detection: {enable: true}, tracker: {enable: true, initiate: {M:3,N:6}, delete:3}},
truth: {adsb: {enabled: false, tar1090: 'auto'}}, location: {rx:{name:'RX',latitude:1,longitude:1,altitude:1},tx:{name:'TX',latitude:2,longitude:2,altitude:2}},
save: {path:'/tmp/',map:false,detection:false}, network:{ip:'127.0.0.1',ports:{api:3000,map:3001,detection:3002,track:3003,timestamp:4000,timing:4001,iqdata:4002}}};
const server = http.createServer((request, response) => {
  const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
  if (pathname === '/api/config' || pathname === '/api/runtime/config') {
    response.setHeader('ETag', '"test"'); response.setHeader('Content-Type','application/json'); response.end(JSON.stringify(config)); return;
  }
  if (pathname === '/api/config/capabilities') {
    response.setHeader('Content-Type','application/json'); response.end(JSON.stringify({editable:true,restartAvailable:false,configRevision:'test',deviceProfiles:[],fieldRules:{}})); return;
  }
  if (pathname.startsWith('/api/') || pathname.startsWith('/capture/')) {
    response.setHeader('Content-Type','application/json'); response.end('{}'); return;
  }
  const filename = path.resolve(root, '.' + (pathname.endsWith('/') ? pathname + 'index.html' : pathname));
  if (!filename.startsWith(root + path.sep) || !fs.existsSync(filename) || !fs.statSync(filename).isFile()) { response.writeHead(404); response.end(); return; }
  response.setHeader('Content-Type', {'.js':'application/javascript','.css':'text/css','.html':'text/html','.svg':'image/svg+xml'}[path.extname(filename)] || 'application/octet-stream');
  fs.createReadStream(filename).pipe(response);
});
(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch({headless:true, ...(process.env.VECTORWARP_CHROME_EXECUTABLE ? {executablePath:process.env.VECTORWARP_CHROME_EXECUTABLE}: {})});
  try {
    const base = `http://127.0.0.1:${server.address().port}`;
    const routes = ['/controller/','/display/configuration/','/display/recordings/','/display/overview/','/display/statistics/'];
    for (const width of [320, 375, 768, 1440]) {
      const page = await browser.newPage({viewport:{width,height:900}});
      for (const route of routes) {
        await page.goto(base + route, {waitUntil:'domcontentloaded'});
        await page.waitForTimeout(100);
        const overflow = await page.evaluate(() => Math.max(document.documentElement.scrollWidth, document.body.scrollWidth) - innerWidth);
        assert.ok(overflow <= 2, `${route} overflows ${width}px viewport by ${overflow}px`);
        const header = await page.locator('.app-header').boundingBox();
        assert.ok(header && header.width <= width + 2, `${route} header must fit ${width}px`);
      }
      await page.close();
    }
    console.log('Primary pages fit 320, 375, 768 and 1440 pixel browser viewports.');
  } finally { await browser.close(); await new Promise(resolve => server.close(resolve)); }
})().catch(error => { console.error(error); process.exitCode = 1; });
