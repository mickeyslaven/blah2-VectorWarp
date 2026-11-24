'use strict';
// Uses the installed Chromium and Node's WebSocket; no browser package download.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const {spawn} = require('node:child_process');
const {once} = require('node:events');

const root = path.resolve(__dirname, '../../html');
const illuminators = [0, 1, 2, 3].map(index => ({id: `channel-${index}`,
  transmitter_site: `site-${index % 2}`, frequency_hz: 509e6 + index * 6e6,
  available: true}));
const config = {capture: {fc: 509e6, fs: 2.4e6, device: {type: 'Kraken'}},
  process: {data: {cpi: .5}, ambiguity: {delayMin: 0, delayMax: 100,
    dopplerMin: -100, dopplerMax: 100}},
  location: {rx: {latitude: 40, longitude: -80}, tx: {latitude: 41, longitude: -80}},
  truth: {adsb: {enabled: true}}, save: {}};
let radarAvailable = true, associationsAvailable = true, plainVisible = true, envelopeRadius = 4, envelopeOperational = false;
const server = http.createServer((request, response) => {
  const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
  if (pathname.startsWith('/api/') || pathname.startsWith('/capture/')) {
    const data = {
      '/api/system/status': {serverId: 'fusion-layout-fixture', radar: 'receiving', adsb: 'receiving'},
      '/api/runtime/config': config,
      '/api/config': config,
      '/api/illuminators/live': {enabled: true, primary: illuminators[0].id, illuminators},
      '/api/federation/nodes': {nodes: [{id: 'local', label: 'Fixture', local: true}]},
      '/api/federation/cooperative': {tracks: Array.from({length: 64}, () => ({contributing_nodes: ['local']}))},
      '/api/tracker/fused': {available: radarAvailable, tracks: [{id:'F1',state:'FUSED_ACTIVE',local_east_km:12,local_north_km:16,
        velocity_east_km_s:.1,velocity_north_km_s:.1,contributors:[{illuminator:'channel-0'},{illuminator:'channel-1'}]}],
        position_candidates:[{id:'E1',state:'ENVELOPE_SUPPORTED',operational_position:envelopeOperational,
          representative_is_fix:envelopeOperational,resolved:envelopeOperational,latitude:40.1,longitude:-80.1,
          position_envelope:{horizontal_radius_km:envelopeRadius}}],
        validation:{held_out_truth:true,adsb_used_by_solver:false,target_kind:'operational-fused-tracks',
          matches:[{candidate_id:'F1',adsb_hex:'matched',adsb_flight:'RADAR123'}]},
        cross_frequency_support: {tracks: [{id: 'X6010690', state: 'SUPPORT_ACTIVE',
          diagnostic_only: true, supporting_cpis: 2}]}},
      '/api/adsb': {aircraft: [
        {hex:'plain',flight:'PLAIN123',lat:40.2,lon:-80.1,gs:140,track:45,alt_baro:30000},
        {hex:'detected',flight:'DETECT123',lat:40.1,lon:-79.9,track:90},
        {hex:'tracked',flight:'TRACK123',lat:40.3,lon:-79.8,track:180},
        {hex:'matched',flight:'RADAR123',lat:40.15,lon:-79.9,track:15}].filter(item=>plainVisible||item.hex!=='plain')},
      '/api/live-aircraft-associations': {associations: associationsAvailable ? [
        {hex:'detected',detected:true,channels:['channel-0','channel-1']},
        {hex:'tracked',tracked:true,channels:['channel-0']}] : []},
      '/api/federation/live-aircraft-associations': {associations: associationsAvailable ? [
        {hex:'detected',detected:true,channels:['channel-0','channel-1']},
        {hex:'tracked',tracked:true,channels:['channel-0']}] : []}
    }[pathname] || {};
    response.setHeader('Content-Type', 'application/json');
    response.end(JSON.stringify(data)); return;
  }
  const filename = path.resolve(root, '.' + (pathname.endsWith('/') ? pathname + 'index.html' : pathname));
  if (!filename.startsWith(root + path.sep) || !fs.existsSync(filename) || !fs.statSync(filename).isFile()) {
    response.writeHead(404); response.end(); return;
  }
  response.setHeader('Content-Type', {'.js': 'application/javascript', '.mjs': 'application/javascript', '.css': 'text/css',
    '.html': 'text/html', '.svg': 'image/svg+xml'}[path.extname(filename)] || 'application/octet-stream');
  if (filename.endsWith('.html')) {
    // Discovery must never fall back to a real API on the default port.
    response.end(fs.readFileSync(filename, 'utf8').replace('BLAH2_API_PORT="3000"',
      `BLAH2_API_PORT="${server.address().port}"`).replace('<head>',`<head><script>window.BLAH2_API_PORT="${server.address().port}";</script>`));
  } else fs.createReadStream(filename).pipe(response);
});

async function connect(endpoint) {
  const socket = new WebSocket(endpoint);
  await once(socket, 'open');
  let id = 0;
  const pending = new Map();
  const events = new Map();
  socket.addEventListener('message', event => {
    const message = JSON.parse(event.data);
    if (message.method) { events.get(message.method)?.(message.params); return; }
    const request = pending.get(message.id);
    if (!request) return;
    pending.delete(message.id); clearTimeout(request.timeout);
    if (message.error) request.reject(new Error(JSON.stringify(message.error)));
    else request.resolve(message.result);
  });
  return {socket, on(method,handler) {events.set(method,handler);}, send(method, params = {}) {
    return new Promise((resolve, reject) => {
      const requestId = ++id;
      const timeout = setTimeout(() => {pending.delete(requestId); reject(new Error(`${method} timed out`));}, 15000);
      pending.set(requestId, {resolve, reject, timeout});
      socket.send(JSON.stringify({id: requestId, method, params}));
    });
  }};
}

(async () => {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-fusion-responsive-'));
  let browser, client;
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    browser = spawn(process.env.VECTORWARP_CHROME_EXECUTABLE || '/usr/bin/chromium-browser',
      ['--headless', '--no-sandbox', '--disable-gpu', '--enable-unsafe-swiftshader', '--disable-dev-shm-usage', '--disable-background-networking',
        '--disable-extensions', '--no-first-run', '--remote-debugging-port=0',
        `--user-data-dir=${profile}`, 'about:blank'], {stdio: ['ignore', 'ignore', 'pipe']});
    const endpoint = await new Promise((resolve, reject) => {
      let stderr = '';
      const timeout = setTimeout(() => reject(new Error(`Chromium startup timed out: ${stderr.slice(-2000)}`)), 30000);
      browser.once('error', error => {clearTimeout(timeout); reject(error);});
      browser.once('exit', code => {clearTimeout(timeout); reject(new Error(`Chromium exited ${code}: ${stderr.slice(-2000)}`));});
      browser.stderr.on('data', chunk => {
        stderr += chunk;
        const match = stderr.match(/DevTools listening on (ws:\/\/[^\s]+)/);
        if (match) {clearTimeout(timeout); resolve(match[1]);}
      });
    });
    const targets = await (await fetch(`http://${new URL(endpoint).host}/json/list`)).json();
    client = await connect(targets.find(target => target.type === 'page').webSocketDebuggerUrl);
    const evaluate = async expression => {
      const result = await client.send('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true});
      assert.ok(!result.exceptionDetails, JSON.stringify(result.exceptionDetails));
      return result.result.value;
    };
    const viewport = (width, height) => client.send('Emulation.setDeviceMetricsOverride',
      {width, height, deviceScaleFactor: 1, mobile: false});
    const base = `http://127.0.0.1:${server.address().port}`;
    const ready = () => evaluate(`new Promise((resolve, reject) => {
      const started = Date.now();
      const check = () => {
        const plot = document.getElementById('fusion-scope');
        const states = new Set((plot?.data || []).filter(trace => trace.meta?.kind === 'aircraft')
          .map(trace => trace.meta.positionSource + ':' + trace.meta.evidenceState));
        if (plot?._fullLayout && plot.layout?.images?.length === 4 &&
            states.has('adsb:detected') && states.has('adsb:tracked') && states.has('radar:tracked') &&
            document.querySelector('#fusion-diagnostics h2')) {
          resolve(true);
        } else if (Date.now() - started > 15000) reject(new Error('Fusion fixture did not render'));
        else setTimeout(check, 50);
      }; check();
    })`);
    const measure = () => evaluate(`(() => {
      const box = selector => {
        const element = document.querySelector(selector), rect = element.getBoundingClientRect();
        return {x: rect.x, y: rect.y, right: rect.right, bottom: rect.bottom,
          width: rect.width, height: rect.height, visible: !!element.getClientRects().length};
      };
      return {plot: box('#fusion-scope'), svg: box('#fusion-scope .main-svg'),
        details: box('#fusion-diagnostics'), status: box('#fusion-scope-status'),
        toggle: box('#fusion-diagnostics-toggle'), pane: box('.fusion-plot-pane'),
        overflow: document.documentElement.scrollWidth - innerWidth};
    })()`);
    const glyphContract = () => evaluate(`(() => {
      const plot = document.getElementById('fusion-scope');
      const aircraft = plot.data.filter(trace => trace.meta?.kind === 'aircraft');
      return {aircraft:aircraft.map(trace=>({id:trace.meta.aircraftId,source:trace.meta.positionSource,
        state:trace.meta.evidenceState,mode:trace.mode,x:trace.x})),
        images:plot.layout.images.map(item=>({name:item.name,svg:decodeURIComponent(item.source.split(',').slice(1).join(','))})),
        renderedImages:plot.querySelectorAll('.imagelayer image').length,
        trails:plot.data.filter(trace=>trace.meta?.kind==='aircraft-trail').map(trace=>trace.mode),
        diagnostics:plot.data.filter(trace=>trace.meta?.kind==='diagnostic').length,
        rows:document.querySelectorAll('.fusion-aircraft-row').length,
        paragraphs:document.querySelectorAll('.fusion-evidence-note').length,
        support:document.querySelector('.fusion-glance-board').textContent,
        transition:plot.layout.transition.duration};
    })()`);
    function assertGlyphContract(contract) {
      assert.equal(contract.aircraft.length,4);
      assert.equal(contract.images.length,4,'one canonical glyph image per aircraft');
      assert.equal(contract.renderedImages,4,'browser paints one image per aircraft');
      assert.equal(new Set(contract.aircraft.map(value=>value.id)).size,4);
      assert.ok(contract.aircraft.every(value=>value.mode==='text'),'no point-marker shapes under aircraft glyphs');
      assert.ok(contract.trails.every(mode=>mode==='lines'),'history never adds marker shapes');
      assert.equal(contract.rows,4); assert.equal(contract.paragraphs,0);
      assert.equal(contract.diagnostics,0,'diagnostics default off');
      assert.match(contract.support,/2 channels/);
      const plain=contract.images.find(value=>value.name==='adsb:plain').svg;
      assert.match(plain,/fill="#8f66e5"/); assert.match(plain,/stroke="#8f66e5"/,'canonical 30,000 ft altitude color');
      assert.match(contract.images.find(value=>value.name==='adsb:detected').svg,/fusion-evidence detected/);
      assert.match(contract.images.find(value=>value.name==='adsb:tracked').svg,/fusion-evidence tracked/);
      assert.match(contract.images.find(value=>value.name==='radar:F1').svg,/fusion-evidence fused/);
      assert.ok(contract.images.every(value=>value.svg.includes('fill-opacity="1"')),'altitude fill stays consistent across evidence states');
      assert.ok(contract.images.every(value=>!value.svg.includes('aircraft-3d-depth')),'no duplicate depth silhouette');
      assert.equal(contract.transition,0);
    }
    for (const width of [320, 390, 768]) {
      await viewport(width, 844);
      await client.send('Page.navigate', {url: `${base}/?radar_view=fusion&fusion_details=open`});
      await ready();
      assertGlyphContract(await glyphContract());
      for (const expanded of [true, false, true]) {
        if (await evaluate(`document.getElementById('fusion-diagnostics-toggle').getAttribute('aria-expanded') !== '${expanded}'`)) {
          await evaluate(`document.getElementById('fusion-diagnostics-toggle').click()`);
          await evaluate(`new Promise(resolve => setTimeout(resolve, 200))`);
        }
        const bounds = await measure();
        assert.ok(bounds.plot.height >= 320, `${width}px chart needs reserved height: ${JSON.stringify(bounds)}`);
        assert.ok(bounds.svg.bottom <= bounds.plot.bottom + 2, `${width}px Plotly SVG escapes its chart`);
        assert.ok(bounds.status.bottom <= bounds.plot.y + 1, `${width}px status overlaps chart`);
        assert.equal(bounds.details.visible, expanded, `${width}px details toggle`);
        if (expanded) assert.ok(bounds.details.y >= bounds.svg.bottom - 1, `${width}px details overlap Plotly SVG`);
        assert.ok(bounds.overflow <= 2, `${width}px horizontal overflow: ${bounds.overflow}`);
      }
    }
    await viewport(1440, 900);
    await client.send('Page.navigate', {url: `${base}/?radar_view=fusion&fusion_details=open`});
    await ready();
    const desktop = await measure();
    assert.ok(desktop.details.x >= desktop.plot.right - 1, 'desktop details stay beside the scope');
    await evaluate(`document.querySelector('button[data-aircraft-id="adsb:detected"]').focus()`);
    assert.equal(await evaluate(`document.activeElement.dataset.aircraftId`),'adsb:detected');
    await client.send('Input.dispatchKeyEvent',{type:'keyDown',key:'Enter',code:'Enter',text:'\r',unmodifiedText:'\r',windowsVirtualKeyCode:13});
    await client.send('Input.dispatchKeyEvent',{type:'keyUp',key:'Enter',code:'Enter',windowsVirtualKeyCode:13});
    assert.match(await evaluate(`document.getElementById('fusion-diagnostics').textContent`),/DETECT123.*ADS-B position.*Radar detected/);
    assert.equal(await evaluate(`document.activeElement.dataset.aircraftId`),'adsb:detected');
    await evaluate(`window.statusChanges=0;new MutationObserver(()=>window.statusChanges++).observe(document.getElementById('fusion-scope-status'),{subtree:true,childList:true,characterData:true})`);
    await evaluate(`new Promise(resolve=>setTimeout(resolve,1400))`);
    assert.equal(await evaluate('window.statusChanges'),0,'unchanged polling does not repeat aria status updates');
    assertGlyphContract(await glyphContract());
    await evaluate(`document.getElementById('fusion-hypotheses').click()`);
    await evaluate(`new Promise(resolve=>setTimeout(resolve,500))`);
    assert.equal((await glyphContract()).diagnostics,0,'No unqualified map-support tracker is rendered');
    await client.send('Emulation.setEmulatedMedia',{features:[{name:'prefers-reduced-motion',value:'reduce'}]});
    assert.equal(await evaluate(`getComputedStyle(document.querySelector('.fusion-aircraft-row')).transitionDuration`),'0s');
    associationsAvailable=false;
    await evaluate(`new Promise(resolve=>setTimeout(resolve,1400))`);
    assert.equal((await glyphContract()).aircraft.find(value=>value.id==='adsb:tracked').state,'adsb','missing current association clears stale support');
    associationsAvailable=true;
    radarAvailable=false;
    await evaluate(`new Promise(resolve=>setTimeout(resolve,1400))`);
    const outage=await glyphContract();
    assert.equal(outage.aircraft.length,4,'ADS-B remains visible during radar outage');
    assert.ok(outage.aircraft.every(value=>value.source==='adsb'),'stale independent radar position removed');
    radarAvailable=true;
    await viewport(1080, 1920);
    await client.send('Page.navigate', {url: `${base}/?radar_view=fusion&scope=all&fusion_details=closed&distance_view=1`});
    await ready();
    const kiosk = await measure();
    assert.ok(kiosk.plot.height > 1000, 'portrait distance view keeps its full-height scope');
    assert.equal(kiosk.details.visible, false, 'kiosk details stay closed');
    assert.equal(kiosk.toggle.visible, false, 'kiosk detail toggle stays hidden');
    assertGlyphContract(await glyphContract());
    const kioskState = await evaluate(`(() => {
      const plot = document.getElementById('fusion-scope');
      const metrics = [...document.querySelectorAll('.fusion-glance-metric')];
      const rows = [...document.querySelectorAll('.fusion-aircraft-row')];
      const evidence = selector => {
        const element = document.querySelector(selector), style = getComputedStyle(element);
        return {fill:style.backgroundColor,border:style.borderColor,shape:style.borderRadius};
      };
      return {
        counts:metrics.map(element=>Number(element.querySelector('strong').textContent)),
        countY:metrics.map(element=>element.querySelector('strong').getBoundingClientRect().y),
        ranges:rows.map(element=>parseFloat(element.querySelector('b').textContent)),
        independent:document.querySelector('[data-aircraft-id="radar:F1"] span').textContent,
        independentEvidence:evidence('[data-aircraft-id="radar:F1"] i'),
        independentCounter:evidence('.fusion-glance-metric.radar i'),
        trackedEvidence:evidence('[data-aircraft-id="adsb:tracked"] i'),
        trackedCounter:evidence('.fusion-glance-metric.tracked i'),
        boardBottom:document.querySelector('.fusion-glance-board').getBoundingClientRect().bottom,
        clippedRows:rows.some(element=>element.scrollWidth>element.clientWidth+1),
        rings:plot.layout.annotations.filter(value=>/ km$/.test(value.text)).map(value=>value.text),
        axesRatio:Math.abs(plot._fullLayout.xaxis._m/plot._fullLayout.yaxis._m)
      };
    })()`);
    assert.deepEqual(kioskState.counts,[1,1,1,1],'each aircraft belongs to exactly one visible evidence category');
    assert.equal(kioskState.counts.reduce((total,value)=>total+value,0),4,'counters do not double-count independent radar positions');
    assert.ok(Math.max(...kioskState.countY)-Math.min(...kioskState.countY)<1,'counter baselines align even with a two-line label');
    assert.deepEqual(kioskState.ranges,[...kioskState.ranges].sort((a,b)=>a-b),'kiosk cards remain nearest first despite an active selection');
    assert.equal(kioskState.independent,'Independent radar position');
    assert.deepEqual(kioskState.independentEvidence,kioskState.independentCounter,'independent position card and counter share their evidence ring');
    assert.deepEqual(kioskState.trackedEvidence,kioskState.trackedCounter,'tracked card and counter share their evidence ring');
    assert.notEqual(kioskState.independentEvidence.border,kioskState.trackedEvidence.border,'source-independent radar and ADS-B-position tracking remain visually distinct');
    assert.ok(kioskState.boardBottom<=1920 && !kioskState.clippedRows,'all four nearest cards fit the portrait viewport');
    assert.ok(kiosk.overflow<=2,'kiosk has no horizontal overflow');
    assert.deepEqual(kioskState.rings,['20 km','40 km','60 km'],'all visible range rings are labelled');
    assert.ok(Math.abs(kioskState.axesRatio-1)<.001,'straight-down scope preserves circular range rings');
    const screenshot = async name => {
      if (!process.env.VECTORWARP_SCREENSHOT_DIR) return;
      fs.mkdirSync(process.env.VECTORWARP_SCREENSHOT_DIR, {recursive: true});
      const capture = await client.send('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false});
      fs.writeFileSync(path.join(process.env.VECTORWARP_SCREENSHOT_DIR, name), Buffer.from(capture.data, 'base64'));
    };
    await screenshot('fusion-portrait-1080x1920.png');
    plainVisible = false;
    await evaluate(`new Promise((resolve,reject)=>{
      const started=Date.now();const check=()=>{
        if(document.querySelector('[data-aircraft-id="adsb:plain"] span')?.textContent==='ADS-B only · coasting')resolve(true);
        else if(Date.now()-started>5000)reject(new Error('Kiosk coasting state did not update'));
        else setTimeout(check,100);
      };check();})`);
    assert.equal(await evaluate(`document.getElementById('fusion-scope').layout.images.find(image=>image.name==='adsb:plain').opacity`),.55,
      'coasting card label matches the faded aircraft glyph');
    await screenshot('fusion-portrait-coasting-1080x1920.png');
    plainVisible = true;
    radarAvailable = false;
    await evaluate(`new Promise((resolve,reject)=>{
      const started=Date.now();const check=()=>{
        if(document.querySelector('.fusion-glance-metric.radar strong')?.textContent==='0')resolve(true);
        else if(Date.now()-started>5000)reject(new Error('Kiosk radar outage did not update'));
        else setTimeout(check,100);
      };check();})`);
    assert.deepEqual(await evaluate(`[...document.querySelectorAll('.fusion-glance-metric strong')].map(element=>Number(element.textContent))`),[2,1,1,0],
      'radar outage removes the independent category and restores the external position');
    assert.equal(await evaluate(`document.querySelector('.fusion-data-warning')?.textContent || ''`), '',
      'detected and tracked evidence must not be hidden behind a radar-unavailable warning');
    await screenshot('fusion-portrait-radar-unavailable-1080x1920.png');
    associationsAvailable = false;
    await evaluate(`new Promise((resolve,reject)=>{
      const started=Date.now();const check=()=>{
        if(document.querySelector('.fusion-data-warning')?.textContent==='Radar data unavailable')resolve(true);
        else if(Date.now()-started>5000)reject(new Error('Kiosk true no-evidence warning did not update'));
        else setTimeout(check,100);
      };check();})`);
    assert.deepEqual(await evaluate(`[...document.querySelectorAll('.fusion-glance-metric strong')].map(element=>Number(element.textContent))`),[4,0,0,0],
      'true no-evidence state retains ADS-B positions without inventing radar evidence');
    associationsAvailable = true;
    radarAvailable = true;
    // Exercise the actual geographic Map and its existing 3D aircraft renderer.
    client.on('Fetch.requestPaused', event => {
      const json=event.request.url.endsWith('tilejson.json');
      const body=json?Buffer.from(JSON.stringify({tilejson:'3.0.0',tiles:['https://tiles.example.test/{z}/{x}/{y}.png'],minzoom:0,maxzoom:12})).toString('base64'):
        'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=';
      client.send('Fetch.fulfillRequest',{requestId:event.requestId,responseCode:200,
        responseHeaders:[{name:'Content-Type',value:json?'application/json':'image/png'}],body}).catch(()=>{});
    });
    await client.send('Fetch.enable',{patterns:[{urlPattern:'https://*'}]});
    await viewport(1000,800);
    await client.send('Page.navigate',{url:`${base}/display/locations/`});
    const waitMap = condition => evaluate(`new Promise((resolve,reject)=>{
      const started=Date.now();const check=()=>{if(${condition})resolve(true);
        else if(Date.now()-started>12000)reject(new Error('Map fixture: '+(window.blah2ViewLastError||'not ready')));
        else setTimeout(check,100);};check();})`);
    await waitMap(`document.querySelectorAll('.location-aircraft-marker').length===4 && document.getElementById('data')._vectorwarpMap?.getSource('vectorwarp-fusion-envelopes')`);
    const mapSnapshot = () => evaluate(`(()=>{
      const map=document.getElementById('data')._vectorwarpMap,layer=document.querySelector('.location-aircraft-layer');
      return {markers:[...layer._aircraftMarkers.values()].map(value=>({source:value.marker.dataset.positionSource,
        label:value.label.textContent,lat:value.item.lat,lon:value.item.lon,ring:getComputedStyle(value.marker,'::before').display})),
        envelopes:map.getSource('vectorwarp-fusion-envelopes')._vectorwarpData.features,
        duplicateLayer:!!map.getLayer('vectorwarp-fused-track-hypotheses'),renderer:layer._modelCanvas.dataset.renderer};
    })()`);
    let mapState=await mapSnapshot();
    assert.equal(mapState.markers.filter(value=>value.source==='radar').length,1);
    assert.equal(mapState.markers.filter(value=>value.source==='adsb').length,3);
    assert.equal(mapState.envelopes.length,0,'Map diagnostics default off');
    assert.equal(mapState.duplicateLayer,false,'no second fused point-marker layer');
    assert.equal(mapState.renderer,'vectorwarp-real-3d-v1','independent positions use the existing Map renderer');
    const fusedMarker=mapState.markers.find(value=>value.source==='radar');
    assert.notEqual(fusedMarker.ring,'none','independent fused position has one compact orange evidence ring');
    assert.ok(Math.abs(fusedMarker.lat-(40+16/111.32))<1e-6,'Map uses radar coordinates');
    await evaluate(`document.querySelector('.independent-radar-position .location-aircraft-icon').click()`);
    assert.match(await evaluate(`document.getElementById('selected-aircraft-detail').textContent`),/Independent radar position.*Radar tracked.*Multi-channel support/);
    assert.equal(await evaluate(`document.querySelector('.independent-radar-position .location-aircraft-icon').getAttribute('aria-pressed')`), 'true',
      'Map selection is announced to assistive technology');
    await evaluate(`document.querySelector('.aircraft-detail-close').focus();document.querySelector('.aircraft-detail-close').click()`);
    assert.equal(await evaluate(`document.activeElement===document.querySelector('.independent-radar-position .location-aircraft-icon')`), true,
      'Closing aircraft details returns keyboard focus to the selected icon');
    assert.equal(await evaluate(`document.activeElement.getAttribute('aria-pressed')`), 'false');
    await evaluate(`document.getElementById('show-fusion-envelopes').click()`);
    await waitMap(`document.getElementById('data')._vectorwarpMap.getSource('vectorwarp-fusion-envelopes')._vectorwarpData.features.length===1`);
    mapState=await mapSnapshot();
    assert.equal(mapState.markers.length,4,'unresolved envelope never adds an aircraft point');
    assert.equal(mapState.envelopes[0].geometry.type,'Polygon');
    assert.equal(mapState.envelopes[0].properties.radius_km,4);
    envelopeRadius=1;
    await waitMap(`document.getElementById('data')._vectorwarpMap.getSource('vectorwarp-fusion-envelopes')._vectorwarpData.features[0]?.properties.radius_km===1`);
    envelopeOperational=true;
    await waitMap(`document.querySelectorAll('.location-aircraft-marker').length===5`);
    mapState=await mapSnapshot();
    assert.equal(mapState.envelopes.length,0,'qualified operational fix replaces its diagnostic region');
    assert.equal(mapState.markers.filter(value=>value.source==='radar').length,2);
    console.log('Fusion and geographic Map: canonical single glyphs, radar identity merge, bounded opt-in/narrowed regions, explicit qualified fixes, keyboard/aria/reduced-motion, outages and responsive layouts pass.');
  } finally {
    client?.socket.close();
    if (browser && browser.exitCode === null) {
      const closed = once(browser, 'exit'); browser.kill(); await closed;
    }
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(profile, {recursive: true, force: true});
  }
})().catch(error => {console.error(error); process.exitCode = 1;});
