'use strict';
// Isolated, read-only fixture server. No radio/API discovery or external network.
// Use installed Chromium and Node's WebSocket; no browser download is required.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const {spawn} = require('node:child_process');
const {once} = require('node:events');
const root = path.resolve(__dirname, '../../html');
const pages = fs.readdirSync(root, {recursive:true}).filter(file => file.endsWith('index.html'));
const aliases = [], routes = [];
for (const file of pages) {
  const route = '/' + file.replace(/index\.html$/, '');
  const html = fs.readFileSync(path.join(root, file), 'utf8');
  const redirect = html.match(/location\.replace\(['"]([^'"]+)/);
  if (redirect) aliases.push({route, destination:redirect[1]});
  else routes.push(route);
}
routes.push('/display/statistics/?tab=adsb', '/display/diagnostics/?view=timing',
  '/display/diagnostics/?view=health', '/display/signal/?view=maxhold',
  '/display/detection/?view=range', '/display/detection/?view=doppler');
const selectedRoutes=process.env.VECTORWARP_LAYOUT_ROUTES?JSON.parse(process.env.VECTORWARP_LAYOUT_ROUTES):routes;
assert.equal(pages.length, 24, 'Inventory includes all canonical and compatibility HTML pages');

const config = {capture:{fc:509e6,fs:2.4e6,device:{type:'Kraken',channel_count:5,
  gain:[10,10,10,10,10],array_geometry:{shape:'regular_polygon',radius_m:.19}}},
  process:{data:{cpi:.5,buffer:2},performance:{acceleration:'cpu'},
    ambiguity:{delayMin:0,delayMax:100,dopplerMin:-100,dopplerMax:100},
    clutter:{enable:true},detection:{enable:true,pfa:.001},
    tracker:{enable:true,initiate:{M:3,N:6},delete:3},spectrum:{enable:true}},
  truth:{adsb:{enabled:true,tar1090:'auto'}},
  location:{rx:{name:'Layout fixture receiver',latitude:40,longitude:-80,altitude:250},
    tx:{name:'Layout fixture transmitter',latitude:40.2,longitude:-80.1,altitude:400}},
  save:{path:'/tmp/layout-fixture/',map:false,detection:false},
  network:{ip:'127.0.0.1',ports:{api:3000,map:3001,detection:3002,track:3003,
    timestamp:4000,timing:4001,iqdata:4002}}};
const period = {passes:10,eligible_passes:8,detected_passes:6,tracked_passes:4,
  detection_yield:.75,tracking_yield:.5,out_of_config_passes:2,
  eligible_no_radar_return:2,detected_not_tracked:2,
  out_of_span:{below_detection_floor:1,above_delay_ceiling:1,outside_doppler_span:1},
  receiver_slant_range_strata:{bands:{'0–30':{eligible_passes:8,detected_passes:6,tracked_passes:4,tracking_yield:.5}}}};
const scorecard = {decision_grade:true,collecting:true,limits:{delayMinKm:1,delayMaxKm:100,
  dopplerMinHz:-100,dopplerMaxHz:100,clutterMaxKm:5,matchDelayKm:.5,matchDopplerHz:5},
  windows:Object.fromEntries(['hour','day','week','lifetime'].map(name=>[name,period])),
  data_quality:{exact_timed_cpi_frames:100,unique_cpi_frames:100},
  active_passes:[{flight:'LAYOUT123',status:'eligible',eligible_cpis:5,detected_cpis:4,
    tracked_cpis:3,last_truth:{delay_km:12,doppler_hz:45}}],recent_finalized_passes:[]};
function fixture(url) {
  if (['statistics-kiosk','statistics-single-kiosk'].includes(process.env.VECTORWARP_LAYOUT_FIXTURE)) {
    const result = require('./statistics-kiosk-fixture.cjs')(url,config,
      process.env.VECTORWARP_LAYOUT_FIXTURE === 'statistics-single-kiosk');
    if (result !== undefined) return result;
  }
  const timestamp = Date.now();
  return {
    '/api/config':config, '/api/runtime/config':config,
    '/api/config/capabilities':{editable:true,restartAvailable:false,
      configRevision:'layout-fixture',deviceProfiles:[],fieldRules:{}},
    '/api/system/status':{serverId:'layout-fixture',radar:'receiving',adsb:'receiving',adsbEnabled:true},
    '/capture/status':{supported:false,available:false,recording:false},
    '/api/federation/nodes':{nodes:[{id:'local',label:'Layout fixture',local:true}]},
    '/api/timing':{timestamp,nCpi:10,uptime_s:50,cpi_data_ms:500,cpi_hop_ms:500,
      capture_backlog_ms:0,ambiguity_processing:20,detector:1,tracker:1,cpi:35},
    '/api/detection':{timestamp,delay:[1,2,3],doppler:[10,20,30],snr:[12,13,14]},
    '/api/tracker':{timestamp,n:3,nActive:1,nAssociated:1,nCoasting:1,nTentative:0,
      data:['ACTIVE','ASSOCIATED','COASTING'].map((state,index)=>({id:`T${index}`,state,
        delay:index+1,doppler:10,n:3,associated_delay:[1,2,3],associated_doppler:[8,9,10],
        associated_state:['TENTATIVE','ASSOCIATED',state]}))},
    '/api/adsb':{now:timestamp/1000,aircraft:[{hex:'abc123',flight:'LAYOUT123',lat:40.1,
      lon:-80.05,alt_geom:5000,seen_pos:0,track:45}]},
    '/api/live-scorecard':{...scorecard,updated_at:timestamp},
    '/api/live-scorecard/provisional':{...scorecard,updated_at:timestamp},
    '/api/array-calibration/status':{frequency_hz:509e6,corpus:{enabled:true},reference:{samples:10},
      bearing:{automation:{enabled:true,state:'collecting'},progress:{samples:20,
        independent_passes:4,unique_aircraft:3,covered_sectors:2,required_covered_sectors:6,
        evidence_completion_fraction:.3,requirements:[],sectors:[],next_actions:[]}},
      detection:{automation:{enabled:false,state:'disabled'},progress:{accepted_samples:0,
        positive_control:{fit_ready:false},validation:{ready:false},production_replay:{}}}},
    '/api/recordings':{total:3,offset:0,limit:24,storage:{recording_bytes:3072,free_bytes:1e10},
      retention:{automaticCleanup:false,minimumFreeGb:5},recordings:Array.from({length:3},(_,index)=>({
        id:`layout-${index}`,filename:`aircraft-layout-fixture-${index}.blah2iq`,
        replay_file:`/tmp/layout-fixture/${index}.blah2iq`,title:`LAYOUT${index}`,
        created_at:timestamp-index,duration_seconds:4,size_bytes:1024,frequency_hz:509e6,
        receiver:'Kraken',channels:5,kind:'manual',complete:true,aircraft:[]}))}
  }[url.pathname] || {};
}
const server = http.createServer((request,response)=>{
  const url = new URL(request.url,'http://127.0.0.1');
  if (url.pathname.startsWith('/api/') || url.pathname.startsWith('/capture/')) {
    if(request.method!=='GET') {
      response.writeHead(405,{'Content-Type':'application/json'});
      response.end(JSON.stringify({error:'Read-only layout fixture'}));return;
    }
    response.setHeader('Content-Type','application/json');
    response.setHeader('ETag','"layout-fixture"'); response.end(JSON.stringify(fixture(url))); return;
  }
  let filename = path.resolve(root,'.'+url.pathname);
  if (!filename.startsWith(root+path.sep) && filename!==root) {response.writeHead(404);response.end();return;}
  if (fs.existsSync(filename) && fs.statSync(filename).isDirectory()) filename=path.join(filename,'index.html');
  if (!fs.existsSync(filename)) {response.writeHead(404);response.end();return;}
  response.setHeader('Content-Type',{'.html':'text/html','.js':'application/javascript',
    '.mjs':'application/javascript','.css':'text/css','.svg':'image/svg+xml','.json':'application/json',
    '.png':'image/png','.woff2':'font/woff2'}[path.extname(filename)]||'application/octet-stream');
  if (filename.endsWith('.html')) response.end(fs.readFileSync(filename,'utf8')
    .replaceAll('BLAH2_API_PORT="3000"',`BLAH2_API_PORT="${server.address().port}"`)
    .replace(/<head>/,`<head><script>window.BLAH2_API_PORT="${server.address().port}";</script>`));
  else fs.createReadStream(filename).pipe(response);
});
async function connect(endpoint) {
  const socket=new WebSocket(endpoint); await once(socket,'open');
  let id=0; const pending=new Map(),events=new Map();
  socket.addEventListener('message',event=>{
    const value=JSON.parse(event.data); if(value.method){events.get(value.method)?.(value.params);return;}
    const item=pending.get(value.id); if (!item) return;
    pending.delete(value.id);clearTimeout(item.timeout);
    value.error?item.reject(new Error(JSON.stringify(value.error))):item.resolve(value.result);
  });
  return {socket,on(method,callback){events.set(method,callback);},send(method,params={}) {return new Promise((resolve,reject)=>{
    const next=++id,timeout=setTimeout(()=>{pending.delete(next);reject(new Error(`${method} timed out`));},30000);
    pending.set(next,{resolve,reject,timeout});socket.send(JSON.stringify({id:next,method,params}));
  });}};
}
(async()=>{
  const profile=fs.mkdtempSync(path.join(os.tmpdir(),'vectorwarp-all-routes-'));
  let browser,client; const report={pages:pages.length,aliases,checks:[],issues:[]};
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  try {
    browser=spawn(process.env.VECTORWARP_CHROME_EXECUTABLE||'/usr/bin/chromium-browser',
      ['--headless','--no-sandbox','--disable-gpu','--enable-unsafe-swiftshader','--disable-dev-shm-usage',
        '--disable-background-networking','--disable-extensions','--no-first-run','--remote-debugging-port=0',
        `--user-data-dir=${profile}`,'about:blank'],{stdio:['ignore','ignore','pipe']});
    const endpoint=await new Promise((resolve,reject)=>{
      let stderr='';const timeout=setTimeout(()=>reject(new Error(`Chromium startup: ${stderr.slice(-1000)}`)),30000);
      browser.on('error',reject);browser.stderr.on('data',chunk=>{
        stderr+=chunk;const match=stderr.match(/DevTools listening on (ws:\/\/\S+)/);
        if(match){clearTimeout(timeout);resolve(match[1]);}
      });
    });
    const targets=await(await fetch(`http://${new URL(endpoint).host}/json/list`)).json();
    client=await connect(targets.find(item=>item.type==='page').webSocketDebuggerUrl);
    await client.send('Page.enable');
    client.on('Fetch.requestPaused',event=>{
      const json=event.request.url.endsWith('tilejson.json');
      const body=json?Buffer.from(JSON.stringify({tilejson:'3.0.0',tiles:['https://tiles.example.test/{z}/{x}/{y}.png'],minzoom:0,maxzoom:12})).toString('base64'):
        'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=';
      client.send('Fetch.fulfillRequest',{requestId:event.requestId,responseCode:200,
        responseHeaders:[{name:'Content-Type',value:json?'application/json':'image/png'}],body}).catch(()=>{});
    });
    await client.send('Fetch.enable',{patterns:[{urlPattern:'https://*'}]});
    const evaluate=async expression=>{const value=await client.send('Runtime.evaluate',
      {expression,returnByValue:true,awaitPromise:true});
      if(value.exceptionDetails)throw new Error(JSON.stringify(value.exceptionDetails));return value.result.value;};
    const base=`http://127.0.0.1:${server.address().port}`;
    const navigate=async(route,requireView=true)=>{
      const loaded=new Promise((resolve,reject)=>{const timeout=setTimeout(()=>reject(new Error(`Load timed out: ${route}`)),30000);
        client.on('Page.loadEventFired',()=>{clearTimeout(timeout);resolve();});});
      await client.send('Page.navigate',{url:base+route});
      await loaded;
      await evaluate(`new Promise((resolve,reject)=>{const start=Date.now();const check=()=>{
        const root=document.querySelector('#view-root'),config=document.querySelector('#configuration');
        if(document.readyState==='complete'&&document.querySelector('.app-nav')&&
          (!${requireView}||((!root||root.dataset.ready==='true')&&(!config||config.querySelector('.config-tabs'))&&
          (!document.querySelector('#recording-list')||document.querySelector('.recording-card')))))
          requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true)));
        else if(Date.now()-start>20000)reject(new Error('Route not ready: '+location.pathname+' '+document.body.innerText.slice(-400)));
        else setTimeout(check,100);};check();})`);
    };
    const measure=()=>evaluate(`(()=>{
      const visible=e=>!!e.getClientRects().length&&getComputedStyle(e).visibility!=='hidden';
      const box=e=>{const r=e.getBoundingClientRect();return {x:r.x,y:r.y,right:r.right,bottom:r.bottom,width:r.width,height:r.height};};
      const nav=[...document.querySelectorAll('.app-nav a')].map(e=>({label:e.textContent,clipped:e.scrollWidth>e.clientWidth+1,...box(e)}));
      const escaped=[...document.querySelectorAll('.app-header,.insight-topbar,.view-state,.controller-intro,.info-card,.metric-card,.data-table-wrap,.view-tabs,.config-tabs,.recording-card,input,select,button,#data .annotation-text')]
        .filter(visible).filter(e=>!e.parentElement?.closest('.data-table-wrap')&&
          !e.closest('.location-aircraft-detail,.maplibregl-control-container'))
        .map(e=>({tag:e.tagName,id:e.id,classes:e.className,text:e.textContent.slice(0,60),...box(e)}))
        .filter(r=>r.right>innerWidth+2||r.x < -2);
      return {width:innerWidth,height:innerHeight,overflow:Math.max(document.documentElement.scrollWidth,document.body.scrollWidth)-innerWidth,
        nav,escaped,current:document.querySelectorAll('.app-nav [aria-current="page"]').length,
        overflowNodes:[...document.querySelectorAll('body *')].filter(visible).filter(e=>e.getBoundingClientRect().right>innerWidth+2)
          .slice(0,12).map(e=>({tag:e.tagName,classes:e.getAttribute('class'),id:e.id,...box(e)})),
        controls:[...document.querySelectorAll('.radar-toolbar > .radar-overlay-control')].filter(visible).map(box),
        recording:[...document.querySelectorAll('.radar-panel > .plot-state,.radar-panel > .recording-control')].filter(visible).map(box),
        h1:document.querySelectorAll('h1').length,tables:document.querySelectorAll('.data-table-wrap').length,
        visibleHeaders:document.querySelectorAll('.app-header').length};})()`);
    const record=async(route,state='default')=>{
      const result=await measure();report.checks.push({route,state,...result});
      if(process.env.VECTORWARP_LAYOUT_SCREENSHOTS&&state==='default'&&[320,720,768,1080,1440].includes(result.width)){
        const shot=await client.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
        fs.writeFileSync(path.join(process.env.VECTORWARP_LAYOUT_SCREENSHOTS,
          `${route.replace(/[^a-z0-9]/gi,'_')}-${result.width}.png`),Buffer.from(shot.data,'base64'));
      }
      if(result.overflow>2||result.escaped.length||result.current!==1||result.visibleHeaders!==1)
        report.issues.push({route,state,...result});
      if(result.width<=760&&new Set(result.nav.map(item=>Math.round((item.y+item.bottom)/2))).size!==1)
        report.issues.push({route,state,width:result.width,reason:'Primary navigation wraps into multiple rows',nav:result.nav});
      if(result.nav.some(item=>item.clipped))
        report.issues.push({route,state,width:result.width,reason:'Primary navigation labels are clipped',nav:result.nav});
      for(let i=0;i<result.controls.length;i++)for(let j=i+1;j<result.controls.length;j++){
        const a=result.controls[i],b=result.controls[j];
        if(Math.min(a.right,b.right)-Math.max(a.x,b.x)>2&&Math.min(a.bottom,b.bottom)-Math.max(a.y,b.y)>2)
          report.issues.push({route,state,width:result.width,reason:'Radar toolbar controls overlap',controls:result.controls});
      }
      if(result.recording.length===2){const [a,b]=result.recording;
        if(Math.min(a.bottom,b.bottom)-Math.max(a.y,b.y)>2)
          report.issues.push({route,state,width:result.width,reason:'Plot status covers recording controls',recording:result.recording});}
      if(['statistics-kiosk','statistics-single-kiosk'].includes(process.env.VECTORWARP_LAYOUT_FIXTURE)&&state==='default'){
        const single=process.env.VECTORWARP_LAYOUT_FIXTURE==='statistics-single-kiosk';
        const kiosk=await evaluate(`(()=>{const view=document.querySelector('.statistics-kiosk-visuals');
          const shown=!!view&&getComputedStyle(view).display!=='none';
          const rect=e=>{const r=e.getBoundingClientRect();return {x:r.x,y:r.y,right:r.right,bottom:r.bottom};};
          return {shown,headlines:[...document.querySelectorAll('.statistics-channel-overview>.statistics-distance-hero strong')].map(e=>e.textContent),
            normalHeadlines:[...document.querySelectorAll('.statistics-page>.insight-stats .metric-card strong')].map(e=>e.textContent),
            legacyPresenceCards:document.querySelectorAll('.cpi-presence-hero .metric-card').length,
            scrollHeight:Math.max(document.documentElement.scrollHeight,document.body.scrollHeight),
            headlineRows:[...document.querySelectorAll('.statistics-channel-overview>.statistics-distance-hero .metric-card')].map(e=>Math.round(e.getBoundingClientRect().top)),
            gaugeRows:[...document.querySelectorAll('.cpi-kiosk-gauge')].map(e=>Math.round(e.getBoundingClientRect().top)),
            internalScroll:shown?[...view.querySelectorAll('.cpi-kiosk-channels,.cpi-kiosk-gauge,.cpi-kiosk-comparison')].filter(e=>e.scrollHeight>e.clientHeight+2).map(e=>({className:e.className,scrollHeight:e.scrollHeight,clientHeight:e.clientHeight})):[],
            horizontalScroll:shown?[...view.querySelectorAll('[class*="cpi-kiosk-"]')].filter(e=>e.scrollWidth>e.clientWidth).map(e=>({className:e.className,scrollWidth:e.scrollWidth,clientWidth:e.clientWidth})):[],
            detectionLabels:[...(view?.querySelectorAll('.cpi-kiosk-bar.detected b')||[])].map(e=>e.textContent),
            gauges:[...document.querySelectorAll('.cpi-kiosk-ring')].map(e=>e.getAttribute('aria-label')),
            channels:document.querySelectorAll('.cpi-kiosk-channel').length,
            comparison:document.querySelector('.cpi-kiosk-comparison')?rect(document.querySelector('.cpi-kiosk-comparison')):null,
            clipped:shown?[...document.querySelectorAll('.statistics-distance-hero strong'),...view.querySelectorAll('h2,h3,strong,b,footer span,.cpi-kiosk-channel header span')]
              .filter(e=>e.scrollWidth>e.clientWidth+2||(['hidden','clip'].includes(getComputedStyle(e).overflowY)&&e.scrollHeight>e.clientHeight+2)||e.getBoundingClientRect().right>innerWidth+2).map(e=>e.textContent):[],
            escapedRings:shown?[...view.querySelectorAll('.cpi-kiosk-ring')].filter(e=>e.getBoundingClientRect().right>e.parentElement.getBoundingClientRect().right).length:0};})()`);
        report.checks[report.checks.length-1].kiosk=kiosk;
        assert.equal(kiosk.shown,new URL(route,'http://fixture').searchParams.get('distance_view')==='1');
        if(!single||kiosk.shown){
          assert.deepEqual(kiosk.headlines,['239','65.3%',single?'54.4%':'83.3%'],'Existing pass headline semantics');
          assert.equal(kiosk.channels,single?1:4,'Every selected channel is charted');
          assert.match(kiosk.gauges[0],/Detected: 65.3%; Missed: 34.7%/);
          assert.match(kiosk.gauges[1],single?/Tracked: 54.4%; Not tracked: 45.6% of eligible aircraft passes/:/Tracked: 83.3%; Not tracked: 16.7% of deduplicated detected aircraft passes/);
          assert.deepEqual(kiosk.detectionLabels,single?['18.7%']:['9.9%','18.7%','16.1%','1.8%'],'Realistic live channel label widths');
          if(single)assert.equal(kiosk.legacyPresenceCards,0,'Single kiosk replaces legacy stretched text cards');
        }else{
          assert.deepEqual(kiosk.normalHeadlines,['239','65.3%','54.4%','0'],'Normal single-channel headline values remain unchanged');
          assert.equal(kiosk.legacyPresenceCards,4,'Normal single-channel CPI detail cards remain available');
        }
        if(kiosk.shown){
          assert.deepEqual(kiosk.clipped,[],`No kiosk label clipping at ${result.width}`);
          assert.equal(kiosk.escapedRings,0,`Gauges stay within cards at ${result.width}`);
          assert.deepEqual(kiosk.horizontalScroll,[],`Every kiosk container fits horizontally at ${result.width}`);
          if(result.width===1080||result.width===720){
            assert.ok(kiosk.comparison.bottom<=result.height, 'Portrait comparison fits the canvas');
            assert.ok(kiosk.comparison.bottom>result.height-90, 'Portrait uses the lower canvas');
            assert.ok(kiosk.scrollHeight<=result.height, `Portrait has no document scrolling at ${result.width}`);
            assert.equal(new Set(kiosk.headlineRows).size,1,'Portrait keeps three headline KPIs together');
            assert.equal(new Set(kiosk.gaugeRows).size,1,'Portrait keeps two gauges side by side');
            assert.deepEqual(kiosk.internalScroll,[],'Portrait shows every channel without internal scrolling');
          }
        }
      }
    };
    for(const route of selectedRoutes){
      await navigate(route);
      for(const [width,height] of [[320,844],[390,844],[720,1280],[768,1024],[1440,900],[1080,1920]]){
        await client.send('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:Number(process.env.VECTORWARP_LAYOUT_DPR)||1,mobile:false});
        await evaluate('new Promise(resolve=>setTimeout(()=>requestAnimationFrame(()=>requestAnimationFrame(resolve)),150))');
        await record(route);
        if(route==='/display/configuration/'){
          const tabs=await evaluate(`[...document.querySelectorAll('.config-tabs button')].map(button=>button.dataset.tab)`);
          for(const tab of tabs){
            await evaluate(`document.querySelector('.config-tabs button[data-tab="${tab}"]').click();
              document.querySelectorAll('.config-tab-panels [role="tabpanel"]:not([hidden]) details').forEach(item=>item.open=true);`);
            await evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))');
            await record(route,`settings:${tab}`);
          }
        }
        const expanded=await evaluate(`(()=>{const items=[...document.querySelectorAll('details.statistics-details,details.statistics-table-details,details.recording-card')];
          items.forEach(item=>item.open=true);return items.length;})()`);
        if(expanded){await record(route,'expanded-details');
          await evaluate(`document.querySelectorAll('details.statistics-details,details.statistics-table-details,details.recording-card').forEach(item=>item.open=false)`);}
      }
      console.log(`Checked ${route} at six viewports`);
    }
    // Compatibility URLs must reach the same tested canonical surfaces.
    for(const alias of process.env.VECTORWARP_LAYOUT_ROUTES?[]:aliases){
      await navigate(alias.route,false);
      const actual=await evaluate('location.pathname+location.search');
      assert.equal(actual.replace(/\/(?=\?|$)/,''),alias.destination.replace(/\/(?=\?|$)/,''),alias.route);
    }
    if(process.env.VECTORWARP_LAYOUT_REPORT)fs.writeFileSync(process.env.VECTORWARP_LAYOUT_REPORT,JSON.stringify(report,null,2));
    assert.equal(report.issues.length,0,JSON.stringify(report.issues.map(({route,state,width,reason,overflow,escaped})=>
      ({route,state,width,reason,overflow,escaped})),null,2));
    console.log(`${selectedRoutes.length} canonical/query views and ${report.checks.length} viewport/state checks passed.`);
  }finally{
    if(process.env.VECTORWARP_LAYOUT_REPORT)fs.writeFileSync(process.env.VECTORWARP_LAYOUT_REPORT,JSON.stringify(report,null,2));
    client?.socket.close();browser?.kill('SIGTERM');
    if(browser&&browser.exitCode===null)await Promise.race([once(browser,'exit'),new Promise(resolve=>setTimeout(resolve,3000))]);
    await new Promise(resolve=>server.close(resolve));
    fs.rmSync(profile,{recursive:true,force:true,maxRetries:3,retryDelay:150});
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
