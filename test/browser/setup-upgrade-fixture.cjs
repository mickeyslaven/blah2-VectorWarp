'use strict';
// In-memory API only. No receiver commands, peer discovery, disk configuration,
// or external network access: this tests the shipped UI, not RF qualification.
const assert = require('node:assert/strict');
const {configurationForReceiver} = require('../../api/config-store');
const {getDeviceProfiles, FIELD_RULES, validateConfig} = require('../../api/config-manager');
const clone = value => JSON.parse(JSON.stringify(value));
const migration = {schema:'vectorwarp-config-migration-v1',id:'browser-upgrade',
  createdAt:'2026-11-23T00:00:00Z',standard:'Current unified radar pipeline',changes:[
    {path:'capture.replay.provenance',beforePresent:false,afterPresent:true,
      after:'legacy-unqualified',reason:'Recorded input requires provenance; live receiver settings are retained.'}]};
function createFixture() {
  let state;
  const reset = (type='Usrp',platform='linux',options={}) => {
    const config=configurationForReceiver(type);
    config.capture.fc=650000000;
    config.process.performance.acceleration='cpu';
    config.location={rx:{name:'Fixture receiver',latitude:1,longitude:2,altitude:10},
      tx:{name:'Fixture transmitter',latitude:1.2,longitude:2.1,altitude:100}};
    if(type==='HackRF')config.capture.device.serial=['0001','0002'];
    if(type==='Usrp')config.capture.fs=24000000;
    config.process.multi_illuminator={enabled:false,illuminators:[]};
    if(options.channelized)config.process.multi_illuminator={enabled:true,illuminators:[{
      id:'fixture-channel',enabled:true,standard:'DVB-T2',frequency_hz:650000000,
      occupied_bandwidth_hz:7000000,output_sample_rate_hz:8000000,filter_taps_per_phase:64,
      atsc_reference_mode:'raw',transmitter:clone(config.location.tx)}]};
    if(options.resident){
      config.capture.device.array_geometry=require('../../html/js/kraken_geometry').presetRecord(config,'pentagon5').geometry;
      Object.assign(config.capture.device.array_geometry,{mapping_confirmed:true,geometry_confirmed:true,
        orientation_confirmed:true,x_axis_bearing_deg_true:0});
      config.process.performance.acceleration='gpu-resident';
      config.process.surveillance_fusion.mode='coherent_beam_bank';
      config.process.bearing.method='covariance_mvdr';config.process.tracker.gpu_capacity=256;
    }
    const validation=validateConfig(config);
    assert.equal(validation.valid,true,validation.errors.join('; '));
    state={config,type,platform,options,revision:1,writes:[],nodes:[],failSave:false,failNode:false,
      directories:[config.save.path,'/fixture/recordings/alternate']};
    return state;
  };
  const evidence = () => ({configMigrations:state.options.migration?[migration]:[],
    ...(state.options.review?{radarPlanReview:{required:true,id:'browser-pin-review',
      message:'The saved receiver configuration is retained. Do not repeat receiver setup.',
      instructions:'Review the radar plan file before starting the radar.',
      contractFile:'/fixture/config.yml.radar-plan.json',
      previewCommand:['node','api/config-store.js','pin-preview','/fixture/installations/neutral-test/portable-configuration/config.yml'],
      acceptCommand:['node','api/config-store.js','accept','/fixture/config.yml']}}:{})});
  return {reset,get state(){return state;},read(url) {
    if(!state)return undefined;
    const config=state.config;
    return {'/api/config':config,'/api/runtime/config':config,
      '/api/config/capabilities':{platform:state.platform,editable:true,restartAvailable:false,
        configRevision:`interaction-${state.revision}`,fieldRules:FIELD_RULES,
        processingBackends:{stages:{resident:{selectable:!!state.options.resident,
          qualified:false,selected:false,executed:false,reason:'Synthetic UI capability; not hardware qualification'}}},
        deviceProfiles:getDeviceProfiles().map(profile=>({...profile,
          liveAvailable:!(state.options.unavailable&&profile.type===state.type)})),
        receiverSynchronization:{pendingIntentValue:'save-pending-v1'},...evidence()},
      '/api/system/status':{serverId:'browser-fixture',radar:'no-data',adsb:'disabled',...evidence()},
      '/api/config/migrations':{receipts:evidence().configMigrations},
      '/api/recordings/directories':{directories:state.directories.map(path=>({path,writable:true}))},
      '/api/federation/nodes':{nodes:[{id:'local',label:'Fixture',local:true},...state.nodes]},
      '/api/upstream/status':{supported:false,matched:false,message:'No live hardware in browser fixture.'},
      '/api/receivers/status':{supported:false},
      '/api/receiver/status':{supported:false}}[url.pathname];
  },async write(request,response,url) {
    if(!state||request.method==='GET')return false;
    let raw='';for await(const chunk of request)raw+=chunk;
    const body=raw?JSON.parse(raw):{};
    const reply=(status,value)=>{response.writeHead(status,{'Content-Type':'application/json'});response.end(JSON.stringify(value));return true;};
    if(url.pathname==='/api/config/validate')return reply(200,validateConfig(body,state.config));
    if(request.method==='PUT'&&url.pathname==='/api/config'){
      assert.equal(request.headers['x-vectorwarp-intent'],'config-write-v1');
      assert.equal(request.headers['if-match'],`"interaction-${state.revision}"`);
      const result=validateConfig(body,state.config);if(!result.valid)return reply(400,{error:result.errors.join('; ')});
      if(state.failSave)return reply(503,{message:'Receiver disconnected. Reconnect the USB cable, then save again.'});
      state.config=clone(body);state.revision++;state.writes.push({url:url.href,config:clone(body)});
      return reply(200,{ok:true,revision:`interaction-${state.revision}`,config:body,
        message:'Settings saved. Run vectorwarp restart to apply them.',restarting:false});
    }
    if(request.method==='POST'&&url.pathname==='/api/federation/nodes'){
      assert.equal(request.headers['x-vectorwarp-intent'],'local-node-add-v1');
      if(body.host!=='192.0.2.10')return reply(400,{error:'Enter the other VectorWarp computer’s IP address.'});
      if(state.failNode)return reply(502,{error:'Node unavailable. Check the address and connection, then try again.'});
      state.nodes=[{id:'fixture-peer',identity:'fixture-peer',label:body.label||'Fixture peer',host:body.host,port:body.port}];
      return reply(200,{nodes:state.nodes});
    }
    if(request.method==='DELETE'&&url.pathname==='/api/federation/nodes/fixture-peer'){
      assert.equal(request.headers['x-vectorwarp-intent'],'local-node-remove-v1');
      state.nodes=[];return reply(200,{ok:true});
    }
    if(request.method==='POST'&&url.pathname==='/api/recordings/directories'){
      assert.equal(request.headers['x-vectorwarp-intent'],'recording-directory-v1');
      assert.match(body.name,/^browser-fixture-/);
      const path=`/fixture/recordings/${body.name}`;state.directories.push(path);return reply(200,{path});
    }
    if(url.pathname==='/api/receivers/discover')return reply(200,{receivers:[],devices:[],message:'No receiver detected. Connect your receiver and retry.'});
    return reply(405,{error:'Browser fixture never executes hardware or system actions.'});
  }};
}

async function runInteractions({fixture,client,evaluate,navigate,record,screenshot,report,profile}) {
  const route='/display/configuration/';
  const observedActions=new Map(),visitedActions=new Map(),withheldActions=new Map();
  // Semantic action identity survives editor replacement; repeated channel rows
  // share handlers, while each visible layout choice has its own title.
  const actionDescriptor=`e=>{const label=(e.getAttribute('aria-label')||e.textContent).replace(/\\s+/g,' ').trim();
    const scope=e.closest('dialog')?.id||e.closest('dialog')?.className||e.closest('[data-path]')?.dataset.path||
      (e.closest('.kraken-geometry')?'kraken-geometry':e.closest('#receiver-setup,#local-radar-nodes,.report-support,.app-header')?.className)||'settings';
    const attrs=['data-wizard-step','data-wizard-next','data-wizard-back','data-wizard-close','data-start-setup',
      'data-wizard-receiver-software','data-site-action','data-site-role','data-channelizer-preset','data-tab']
      .filter(name=>e.hasAttribute(name)).map(name=>name+'='+e.getAttribute(name)).join('|');
    const name=e.id||attrs||(e.classList.contains('geometry-channel-node')?e.querySelector('strong').textContent:
      e.closest('.geometry-preset-card')?e.closest('.geometry-preset-card').querySelector('h4').textContent+' / '+label:label);
    return {key:scope+':'+e.tagName+':'+name,label,disabled:!!e.disabled,href:e.getAttribute('href'),
      target:e.target,rel:e.rel,reason:e.title||e.closest('.config-field')?.textContent||'',tag:e.tagName};}`;
  const observeActions=async()=>{
    const actions=await evaluate(`(()=>{const describe=${actionDescriptor},modal=document.querySelector('dialog[open]');
      return [...document.querySelectorAll('.app-header a,#configuration button,#configuration a,#configuration summary,dialog[open] button,dialog[open] a,dialog[open] summary')]
        .filter(e=>{const r=e.getBoundingClientRect(),closed=e.closest('details:not([open])');
          return (!modal||modal.contains(e))&&r.width>0&&r.height>0&&getComputedStyle(e).visibility!=='hidden'&&
            (!closed||closed.querySelector(':scope > summary')?.contains(e));}).map(describe);})()`);
    for(const action of actions){observedActions.set(action.key,action);
      if(action.disabled)withheldActions.set(action.key,'Disabled in the rendered capability/draft state');
      else if(withheldActions.get(action.key)==='Disabled in the rendered capability/draft state')withheldActions.delete(action.key);}
    return actions;
  };
  let extraControls;
  const wait=async(expression,timeoutMs=10000)=>{
    const deadline=Date.now()+timeoutMs;
    let result;
    while(Date.now()<deadline){
      result=await evaluate(`(()=>({ready:!!(${expression}),detail:[...document.querySelectorAll('#config-message,.config-field-error,.setup-wizard-error')]
        .map(e=>e.textContent).filter(Boolean).join('; ')}))()`);
      if(result.ready)return true;
      await new Promise(resolve=>setTimeout(resolve,40));
    }
    throw new Error(`UI condition timed out at ${await evaluate('location.pathname+location.search')}: ${expression} ${result?.detail||''}`);
  };
  const query=selector=>`document.querySelector(${JSON.stringify(selector)})`;
  const click=async selector=>{
    await wait(query(selector));
    await wait('typeof validationPending==="undefined"||!validationPending');
    const point=await evaluate(`(()=>{const e=${query(selector)};e.scrollIntoView({block:'center',behavior:'instant'});
      if(e.disabled)throw new Error('Disabled action: '+${JSON.stringify(selector)});
      const r=e.getBoundingClientRect(),x=r.x+r.width/2,y=r.y+r.height/2,hit=document.elementFromPoint(x,y);
      if(!hit||!(hit===e||e.contains(hit)))throw new Error('Covered or clipped action: '+${JSON.stringify(selector)}+' '+JSON.stringify({rect:r.toJSON(),hit:hit?.outerHTML.slice(0,300),ancestors:(()=>{const a=[];let p=e;while(p){if(p.hidden||p.tagName==='DETAILS')a.push({tag:p.tagName,hidden:p.hidden,open:p.open,label:p.querySelector(':scope > summary')?.textContent});p=p.parentElement;}return a;})()}));
      const action=e.closest('button,a,summary');return {x,y,action:action?(${actionDescriptor})(action):null};})()`);
    const {x,y}=point;
    await client.send('Input.dispatchMouseEvent',{type:'mousePressed',button:'left',clickCount:1,x,y});
    await client.send('Input.dispatchMouseEvent',{type:'mouseReleased',button:'left',clickCount:1,x,y});
    if(point.action){visitedActions.set(point.action.key,'Real browser click');
      report.actionClicks=[...visitedActions].map(([action,result])=>({action,result}));}
  };
  const text=async(selector,value)=>{await click(selector);await evaluate(`${query(selector)}.select()`);
    await client.send('Input.insertText',{text:String(value)});};
  const select=async(selector,value)=>{await click(selector);await client.send('Input.dispatchKeyEvent',{type:'keyDown',key:'Escape',code:'Escape',windowsVirtualKeyCode:27});
    await client.send('Input.dispatchKeyEvent',{type:'keyUp',key:'Escape',code:'Escape',windowsVirtualKeyCode:27});
    await evaluate(`(()=>{const e=${query(selector)},o=[...e.options].find(o=>o.value===${JSON.stringify(value)});
      if(!o||o.disabled)throw new Error('Unavailable choice '+${JSON.stringify(value)});e.value=o.value;e.dispatchEvent(new Event('change',{bubbles:true}));})()`);};
  const button=async(scope,label)=>{
    const id=await evaluate(`(()=>{const e=[...document.querySelectorAll(${JSON.stringify(scope+' button')})].find(e=>e.textContent.trim()===${JSON.stringify(label)});if(!e)throw new Error('Missing button '+${JSON.stringify(label)});e.dataset.browserAction='target';return '[data-browser-action="target"]';})()`);
    await click(id);await evaluate(`document.querySelectorAll('[data-browser-action]').forEach(e=>delete e.dataset.browserAction)`);
  };
  const field=path=>`[data-path="${path}"] input`;
  const tab=async name=>{await click(`.config-tabs [data-tab="${name}"]`);};
  const reveal=async selector=>{
    const category=await evaluate(`${query(selector)}?.closest('[role="tabpanel"]')?.dataset.tab`);
    if(category)await tab(category);
    const closed=await evaluate(`(()=>{const e=${query(selector)};if(!e)throw new Error('Missing field '+${JSON.stringify(selector)});
      document.querySelectorAll('[data-browser-details]').forEach(item=>delete item.dataset.browserDetails);
      const list=[];let p=e.parentElement;while(p){if(p.tagName==='DETAILS'&&!p.open){p.dataset.browserDetails=String(list.length);list.push('[data-browser-details="'+list.length+'"] > summary');}p=p.parentElement;}return list.reverse();})()`);
    for(const item of closed)await click(item);
  };
  const check=async name=>{await observeActions();await record(await evaluate('location.pathname+location.search'),`interaction:${name}`);await screenshot(`interaction-${name}`);
    assert.deepEqual(await evaluate(`[...document.querySelectorAll('.kraken-geometry .config-group')]
      .filter(group=>!group.querySelector('.config-field,input,select,button')).map(group=>group.querySelector('summary')?.textContent)`),
    [],`${name}: array refresh cannot leave empty disclosures`);
    assert.equal(await evaluate('!!document.querySelector(".kraken-geometry-standalone:not([open])")'),false,
      `${name}: standalone array controls remain reachable without a disclosure button`);
    const dialogs=await evaluate(`[...document.querySelectorAll('dialog[open]')].map(e=>{const r=e.getBoundingClientRect();return {
      id:e.id||e.className,contained:r.left>=-2&&r.right<=innerWidth+2&&r.top>=-2&&r.bottom<=innerHeight+2,
      overflow:e.scrollWidth-e.clientWidth,focus:e.contains(document.activeElement)};})`);
    assert.ok(dialogs.every(item=>item.contained&&item.overflow<=2&&item.focus),`${name}: modal bounds, scrolling and focus ${JSON.stringify(dialogs)}`);
    report.interactions.push(name);console.log(`Checked interaction ${name}`);};
  const start=async(type,platform,width,options={})=>{
    fixture.reset(type,platform,options);
    await client.send('Emulation.setDeviceMetricsOverride',{width,height:900,deviceScaleFactor:1,mobile:false});
    // A new isolated case discards its predecessor's in-memory edits only.
    await evaluate('window.onbeforeunload=null;if(location.protocol==="http:"){localStorage.clear();sessionStorage.clear()}');await navigate(route);
    await wait('!validationPending&&!upstreamPending');
  };
  const popup=async selector=>{
    const href=await evaluate(`${query(selector)}.href`);
    assert.equal(new URL(href).origin,await evaluate('location.origin'),'Only fixture-local popups may navigate');
    await click(selector);
    const deadline=Date.now()+10000;let target;
    while(Date.now()<deadline){
      target=(await client.send('Target.getTargets')).targetInfos.find(item=>item.type==='page'&&item.url===href);
      if(target)break;await new Promise(resolve=>setTimeout(resolve,50));
    }
    assert.ok(target,`Local popup opened: ${href}`);await client.send('Target.closeTarget',{targetId:target.targetId});
  };
  const actionCensus=async()=>{
    report.actionInventory={visited:[],notExecuted:[],unexplained:[]};
    const selectorFor=async key=>evaluate(`(()=>{document.querySelectorAll('[data-browser-census]').forEach(e=>delete e.dataset.browserCensus);
      const describe=${actionDescriptor},e=[...document.querySelectorAll('button,a,summary')].find(e=>describe(e).key===${JSON.stringify(key)});
      if(!e)return null;e.dataset.browserCensus='target';return '[data-browser-census="target"]';})()`);
    const discard=async()=>{if(await evaluate(`!${query('#config-reset')}.disabled`))await click('#config-reset');await wait('!validationPending');};
    const nativeDialog=(pattern,promptText)=>client.on('Page.javascriptDialogOpening',event=>{
      assert.match(event.message,pattern,'Only an expected fixture-local confirmation may execute');
      client.send('Page.handleJavaScriptDialog',{accept:true,...(promptText?{promptText}:{})}).catch(()=>{});
    });
    const execute=async action=>{
      const selector=await selectorFor(action.key);assert.ok(selector,`Action remains reachable: ${action.key}`);
      console.log(`Census action ${action.key}`);
      if(action.disabled){assert.equal(await evaluate(`${query(selector)}.disabled`),true);return;}
      await reveal(selector);
      if(action.href){
        const destination=new URL(action.href,await evaluate('location.href'));
        if(destination.origin!==await evaluate('location.origin')){
          assert.equal(action.target,'_blank');assert.match(action.rel,/noopener|noreferrer/);
          withheldActions.set(action.key,'External support link: destination and new-window isolation checked; offline fixture does not contact third parties');return;
        }
        if(action.target==='_blank')return popup(selector);
        await click(selector);await wait(`location.pathname===${JSON.stringify(destination.pathname)}`);await navigate(route);
        await wait('!validationPending&&!upstreamPending');return;
      }
      if(action.key.startsWith('report-dialog:')&&action.label==='Send report'){
        assert.match(await evaluate(`${query('#report-dialog')}.textContent`),/will send diagnostic information to the project maintainer/);
        assert.match(await evaluate(`${query('#report-dialog')}.textContent`),/cancel and file/);
        withheldActions.set(action.key,'Explicit privacy review and Cancel are present; sending to an external maintainer is deliberately not executed');return;
      }
      if(action.tag==='SUMMARY'){await click(selector);return;}
      if(action.key.includes('data-tab=')){
        const previous=await evaluate('document.querySelector(".config-tabs [aria-selected=true]").dataset.tab');
        const requested=await evaluate(`${query(selector)}.dataset.tab`);await click(selector);
        assert.equal(await evaluate('document.querySelector(".config-tabs [aria-selected=true]").dataset.tab'),requested);
        await tab(previous);return;
      }
      if(action.label==='Scan for receivers'){
        await click(selector);await wait('!document.querySelector("#receiver-setup > button").disabled');
        assert.match(await evaluate('document.querySelector("#receiver-setup").textContent'),/Reinstall|receiver|Scan/);return;
      }
      if(action.label==='New folder'){
        nativeDialog(/New folder name inside VectorWarp recording storage/,'browser-fixture-created');await click(selector);
        await wait('document.querySelector(".recording-folder-control select").value==="/fixture/recordings/browser-fixture-created"');
        assert.ok(fixture.state.directories.includes('/fixture/recordings/browser-fixture-created'));
        await extraControls('.recording-folder-control');
        await discard();return;
      }
      if(action.label==='Add local source'){
        const count=await evaluate('document.querySelectorAll("[data-adsb-source=local]").length');await click(selector);
        assert.equal(await evaluate('document.querySelectorAll("[data-adsb-source=local]").length'),count+1);
        await evaluate('document.querySelectorAll("[data-adsb-source=local]")[document.querySelectorAll("[data-adsb-source=local]").length-1].parentElement.dataset.browserExtraRow="true"');
        await extraControls('[data-browser-extra-row]');await observeActions();
        await click('[data-browser-extra-row] button');await discard();return;
      }
      if(action.key.includes('data-site-action=')){
        await click(selector);await wait(query('#site-dialog[open]'));await extraControls('#site-dialog');
        await observeActions();await button('#site-dialog','Cancel');return;
      }
      if(action.label==='Report an issue'){
        await click(selector);await wait(query('#report-dialog[open]'));await extraControls('#report-dialog');
        await button('#report-dialog','View report');await observeActions();
        await client.send('Browser.setPermission',{origin:await evaluate('location.origin'),permission:{name:'clipboard-write'},setting:'denied'});
        await evaluate(`Object.defineProperty(navigator,'clipboard',{configurable:true,value:{writeText:async()=>{throw new DOMException('Clipboard denied','NotAllowedError');}}})`);
        await button('#report-dialog','Copy report');
        await wait('document.querySelector(".report-state").textContent.includes("copy it manually")');
        assert.equal(await evaluate('document.querySelector(".report-preview").hidden'),false);
        await check('clipboard-denied-recovery');
        await evaluate(`delete navigator.clipboard`);
        await client.send('Browser.grantPermissions',{origin:await evaluate('location.origin'),permissions:['clipboardReadWrite','clipboardSanitizedWrite']});
        await button('#report-dialog','Copy report');
        await wait('document.querySelector(".report-state").textContent==="Report copied."');
        for(const item of await observeActions())if(item.href||item.label==='Send report')await execute(item);
        await check('report-privacy-review');await button('#report-dialog','Hide report');await button('#report-dialog','Cancel');return;
      }
      if(action.key.includes('data-channelizer-preset=')){
        const before=await evaluate('JSON.stringify(activeConfig.process.multi_illuminator.illuminators[0])');await click(selector);
        assert.notEqual(await evaluate('JSON.stringify(activeConfig.process.multi_illuminator.illuminators[0])'),before);
        await discard();return;
      }
      if(action.key.includes(':BUTTON:CH')){
        await click(selector);assert.equal(await evaluate(`${query(selector)}.classList.contains('selected')`),true);return;
      }
      if(action.tag==='BUTTON'&&action.key.split(':')[0].split(' ').includes('kraken-geometry')){
        const before=await evaluate('JSON.stringify(activeConfig.capture.device.array_geometry)');
        if(action.label==='Use this layout')nativeDialog(/Replace the unsaved layout/);
        if(action.label==='Remove layout')nativeDialog(/Remove this layout and its measurements from the draft/);
        await click(selector);await wait('!validationPending');
        assert.notEqual(await evaluate('JSON.stringify(activeConfig.capture.device.array_geometry)'),before,`Array action changes the draft: ${action.label}`);
        if(action.label==='Remove layout'){
          await observeActions();await button('.kraken-geometry','Add antenna layout');
          assert.ok(await evaluate('activeConfig.capture.device.array_geometry'));
        }
        if(action.label==='Add measured element'){
          await evaluate('document.querySelector(".geometry-element:last-child").dataset.browserExtraRow="true"');
          await extraControls('[data-browser-extra-row]','Kraken');await observeActions();
          await click('.geometry-element:last-child button');
        }
        if(action.label==='Use this layout')await check('geometry-layout-choice');
        await discard();return;
      }
      throw new Error(`Unclassified visible action: ${action.key}`);
    };
    for(const [type,platform,width,options] of [['Usrp','linux',390,{}],['Kraken','linux',390,{}],['Kraken','linux',1440,{resident:true}],
      ['RspDuo','darwin',1440,{}],['HackRF','darwin',390,{}],['Usrp','linux',390,{channelized:true}]]){
      await start(type,platform,width,options);await extraControls('.local-display-settings');
      for(const name of await evaluate('[...document.querySelectorAll(".config-tabs button")].map(e=>e.dataset.tab)')){
        await tab(name);
        for(let count=0;count<60;count++){
          const summary=await evaluate(`(()=>{const e=[...document.querySelectorAll('[role=tabpanel]:not([hidden]) details:not([open])')]
            .find(e=>!e.parentElement.closest('details:not([open])'));if(!e)return null;
            document.querySelectorAll('[data-browser-expand]').forEach(e=>delete e.dataset.browserExpand);e.dataset.browserExpand='true';return '[data-browser-expand] > summary';})()`);
          if(!summary)break;await click(summary);
        }
        for(const action of await observeActions())if(!visitedActions.has(action.key))await execute(action);
      }
      if(options.channelized)for(const standard of ['ATSC-1.0','ATSC-3.0']){
        await reveal('[data-lane-field="standard"]');await select('[data-lane-field="standard"]',standard);
        const action=(await observeActions()).find(item=>item.key.includes('data-channelizer-preset='));
        assert.ok(action,'Explicit waveform-width preset is reachable');await execute(action);
      }
      // Direct Save uses the real button; the fixture cannot restart hardware.
      await tab('capture');await reveal(field('capture.fc'));await text('[aria-label="Center frequency Hz"]','1');
      await wait(`!${query('#config-save')}.disabled`);await click('#config-save');
      await wait(`${query('#config-message')}.textContent.includes('Settings saved')`);assert.equal(fixture.state.writes.length,1);
      if(await evaluate('!!document.querySelector("#config-message details")')){
        await click('#config-message details > summary');
        assert.equal(await evaluate('document.querySelector("#config-message details").open'),true);
        assert.ok(await evaluate('document.querySelector("#config-message details").textContent.trim().length>7'));
        await click('#config-message details > summary');
        assert.equal(await evaluate('document.querySelector("#config-message details").open'),false);
      }
      await check(`actions-${type}-${options.resident?'resident':options.channelized?'channelized':platform}-${width}`);
    }
  };
  const finalizeActions=()=>{
    report.actionInventory.visited=[...visitedActions].map(([action,result])=>({action,result}));
    report.actionInventory.notExecuted=[...withheldActions].filter(([key])=>!visitedActions.has(key)).map(([action,reason])=>({action,reason}));
    report.actionInventory.unexplained=[...observedActions.keys()].filter(key=>!visitedActions.has(key)&&!withheldActions.has(key));
    assert.deepEqual(report.actionInventory.unexplained,[],'Every visible setup/settings action must be clicked or explicitly guarded');
    console.log(`${report.actionInventory.visited.length} actions clicked; ${report.actionInventory.notExecuted.length} deliberately not executed; zero unexplained actions.`);
  };
  // Inventory uses the actual rendered controls, so adding an editable field
  // expands the gate automatically. Receiver/backend combinations remain the
  // responsibility of the exhaustive DOM/native matrices, not a Cartesian UI run.
  const controlInventory=async(initializeOnly=false)=>{
    const observed=new Map(),visited=new Map((report.reviewControls||[]).map(control=>[control,'Catalog preview control focused, changed and restored before confirmation'])),unavailable=new Map();
    for(const item of report.controlInventory?.visited||[])visited.set(item.control,item.result);
    for(const item of report.controlInventory?.unavailable||[])unavailable.set(item.control,item.reason);
    const controls=scope=>evaluate(`(()=>{const root=${query(scope)};if(!root)return [];
      const selector=e=>{for(const name of ['data-wizard-receiver','data-wizard-fc','data-wizard-fs','data-site',
          'data-wizard-cpi','data-wizard-range','data-wizard-speed','data-wizard-automatic'])
          if(e.hasAttribute(name))return ${JSON.stringify(scope)}+' ['+name+'='+JSON.stringify(e.getAttribute(name))+']';
        if(e.id)return '#'+CSS.escape(e.id);const parts=[];let n=e;
        while(n&&n!==root){parts.unshift(n.tagName.toLowerCase()+':nth-child('+([...n.parentElement.children].indexOf(n)+1)+')');n=n.parentElement;}
        return ${JSON.stringify(scope)}+' > '+parts.join(' > ');};
      return [...root.querySelectorAll('input,select,textarea')].filter(e=>e.type!=='hidden').map(e=>({
        selector:selector(e),path:e.closest('[data-path]')?.dataset.path||'',tag:e.tagName,type:e.type,
        staged:!!e.closest('.station-import'),
        value:e.value,checked:e.checked,disabled:e.disabled,readOnly:e.readOnly,
        label:e.getAttribute('aria-label')||e.title||e.name||e.id,
        reason:e.title||e.closest('.geometry-element')?.querySelector('.geometry-role')?.textContent,
        available:(()=>{for(let n=e;n&&n!==root;n=n.parentElement)
          if(n.getAttribute('role')!=='tabpanel'&&(n.hidden||getComputedStyle(n).display==='none'))return false;return true;})(),
        options:e.options?[...e.options].filter(o=>!o.disabled).map(o=>o.value):[],
        min:e.min,max:e.max,step:e.step}));})()`);
    const identity=(item,type)=>`${item.path.startsWith('capture.device.')||item.path==='capture.fs'?type+':':''}${item.selector}:${item.tag}:${item.type}`;
    const visible=selector=>evaluate(`(()=>{const e=${query(selector)};if(!e||e.disabled||e.readOnly)return false;
      const r=e.getBoundingClientRect(),s=getComputedStyle(e);return r.width>0&&r.height>0&&s.visibility!=='hidden';})()`);
    const mutate=async(item,value)=>{
      if(await evaluate(`(()=>{const e=${query(item.selector)};return e&&${item.type==='checkbox'?'e.checked':'e.value'}===${JSON.stringify(value)};})()`))return;
      await evaluate(`(()=>{const e=${query(item.selector)};window.__inventoryFocused=document.activeElement===e;
        e.addEventListener('focus',()=>window.__inventoryFocused=true,{once:true});})()`);
      if(item.type==='checkbox'){
        const label=await evaluate(`(()=>{const e=${query(item.selector)},l=e.closest('label.switch');
          if(l){l.dataset.inventoryToggle='true';return '[data-inventory-toggle="true"]';}return ${JSON.stringify(item.selector)};})()`);
        if(await evaluate(`${query(item.selector)}.checked`)!==value)await click(label);
      }else if(item.tag==='SELECT')await select(item.selector,value);
      else await text(item.selector,value);
      assert.equal(await evaluate('window.__inventoryFocused'),true,`Control receives focus: ${item.selector}`);
      await evaluate('document.querySelectorAll("[data-inventory-toggle]").forEach(e=>delete e.dataset.inventoryToggle)');
    };
    const alternate=item=>{
      if(item.type==='checkbox')return !item.checked;
      if(item.tag==='SELECT')return item.options.find(value=>value!==item.value&&['manual','fixed'].includes(value))??item.options.find(value=>value!==item.value);
      if(item.type==='number'){
        const value=Number(item.value||0),step=Number(item.step)>0?Number(item.step):1;
        const next=item.max!==''&&value+step>Number(item.max)?value-step:value+step;
        return String(next);
      }
      return item.value+'x';
    };
    const exercise=async(item,type,scope,depth=0)=>{
      const current=(await controls(scope)).find(candidate=>candidate.selector===item.selector);
      if(!current)return;
      item=current;
      const key=identity(item,type);if(visited.has(key))return;
      report.controlInventory.current=key;
      if(item.disabled||item.readOnly){unavailable.set(key,item.reason||item.label||'Read-only capability');return;}
      if(scope==='#config-fields')await reveal(item.selector);
      if(!await visible(item.selector))return;
      observed.set(key,item.label);
      if(item.type==='file'){
        assert.equal(item.label,'Import station JSON file');
        assert.ok(report.interactions.includes('catalog-preview-390'),'File picker exercised by real file import flow');
        visited.set(key,'File import interaction');return;
      }
      const next=alternate(item);
      if(next===undefined){unavailable.set(key,'Only one enabled choice; no alternative value exists');return;}
      const before=new Set((await controls(scope)).filter(e=>!e.disabled&&!e.readOnly&&e.available).map(e=>identity(e,type)));
      const draft=scope==='#config-fields'?await evaluate('JSON.stringify(activeConfig)'):null;
      await mutate(item,next);
      const changed=await evaluate(`(()=>{const e=${query(item.selector)};return e?${item.type==='checkbox'?'e.checked':'e.value'}:null;})()`);
      // Some composite choices deliberately replace their editor immediately.
      const siteChoice=item.selector.startsWith('#site-select-');
      assert.ok(changed===null||changed==next||siteChoice,`Changed control retains input: ${key}: ${changed} != ${next}`);
      if(draft&&item.path&&!item.staged)assert.notEqual(await evaluate('JSON.stringify(activeConfig)'),draft,
        `Editable configuration control must affect the draft: ${key}`);
      const native=await evaluate(`(()=>{const e=${query(item.selector)};return e?{valid:e.validity.valid,message:e.validationMessage}:null;})()`);
      if(native&&!native.valid)assert.ok(native.message,`Native refusal explains the constraint: ${key}`);
      visited.set(key,'Focused and changed; restoration pending');
      report.controlInventory.visited=[...visited].map(([control,result])=>({control,result}));
      if(visited.size%25===0)console.log(`Exercised ${visited.size} editable controls; latest ${key}`);
      if(scope==='#config-fields'){
        await wait('!validationPending');
        assert.equal(await evaluate('validationErrors.length===0||document.getElementById("config-save").disabled'),true,
          `Invalid state cannot save: ${key}`);
        // Exercise newly enabled dependents while their parent choice is active.
        if(depth<2&&item.path!=='capture.device.type')for(const child of await controls(scope)){
          const childKey=identity(child,type);
          if(!before.has(childKey)&&!visited.has(childKey)&&!child.disabled&&!child.readOnly&&child.available&&childKey!==key)
            await exercise(child,type,scope,depth+1);
        }
      }
      // Site choices are MRU-indexed: selecting one deliberately renumbers it.
      // Restore the saved canonical site with Discard, not the old numeric index.
      if(!siteChoice&&await visible(item.selector))await mutate(item,item.type==='checkbox'?item.checked:item.value);
      if(scope==='#config-fields'&&depth===0){
        if(await evaluate(`!${query('#config-reset')}.disabled`))await click('#config-reset');
        await wait('!validationPending');
        assert.deepEqual(JSON.parse(await evaluate('JSON.stringify(activeConfig)')),JSON.parse(draft),
          `Discard/restoration preserves the complete draft: ${key}`);
      }
      visited.set(key,'Focused, changed, validation observed, restored');
    };
    extraControls=async(scope,type='additional')=>{
      const items=await controls(scope);assert.ok(items.length,`Additional form must contain controls: ${scope}`);
      for(const item of items)await exercise(item,type,scope);
      report.controlInventory.visited=[...visited].map(([control,result])=>({control,result}));
      report.controlInventory.unavailable=[...unavailable].filter(([key])=>!visited.has(key)).map(([control,reason])=>({control,reason}));
      report.controlInventory.unvisited=[...observed.keys()].filter(key=>!visited.has(key)&&
        unavailable.get(key)!=='Only one enabled choice; no alternative value exists');
      assert.deepEqual(report.controlInventory.unvisited,[],'Additional forms leave no editable control unexplained');
    };
    report.controlInventory={visited:[],unavailable:[],unvisited:[]};
    if(initializeOnly)return;
    for(const [type,platform,width,options] of [['Usrp','linux',1440,{}],['Kraken','linux',390,{}],
      ['RspDuo','darwin',1440,{}],['HackRF','darwin',390,{}],['Kraken','linux',1440,{resident:true}],
      ['Usrp','linux',390,{channelized:true}]]){
      await start(type,platform,width,options);
      const baseline=await controls('#config-fields');
      assert.ok(baseline.length>30,`${type}: settings inventory must not silently become empty`);
      console.log(`Inventory ${type}/${platform}/${options.resident?'resident':options.channelized?'channelized':'direct'}: ${baseline.length} rendered controls`);
      for(const item of baseline)if(item.available&&!item.disabled&&!item.readOnly)observed.set(identity(item,type),item.label);
      for(const item of baseline)await exercise(item,type,'#config-fields');
      // Dependent array edits can expose a remaining bearing group after its
      // other fields move to the canonical technical controls.
      const bearingDisclosure='.kraken-geometry [data-config-group="process.bearing"] > summary';
      if(await evaluate(`!!${query(bearingDisclosure)}`)){
        const previous=await evaluate('document.querySelector(".config-tabs [aria-selected=true]").dataset.tab');
        await reveal(bearingDisclosure);const open=await evaluate(`${query(bearingDisclosure)}.parentElement.open`);
        await click(bearingDisclosure);assert.equal(await evaluate(`${query(bearingDisclosure)}.parentElement.open`),!open);
        await click(bearingDisclosure);assert.equal(await evaluate(`${query(bearingDisclosure)}.parentElement.open`),open);
        await tab(previous);
      }
      assert.equal(fixture.state.writes.length,0,'Control inventory never writes operational configuration');
      await check(`controls-${type}-${options.resident?'resident':options.channelized?'channelized':platform}-${width}`);
    }
    // Wizard fields use their ordinary native constraints and are restored before
    // a step can save; saved/reloaded step behavior is covered by the flow above.
    for(const step of [0,1,2,3,4,5]){
      await start('Usrp','linux',390);await click('[data-start-setup]');await click(`[data-wizard-step="${step}"]`);
      if(step===5)await click('.setup-wizard-automatic > summary');
      for(const item of await controls('.setup-wizard'))await exercise(item,'wizard','.setup-wizard');
      await check(`wizard-controls-${step}`);await click('[data-wizard-close]');
    }
    await start('Usrp','linux',390);await tab('capture');
    await click('[data-site-action="edit"][data-site-role="rx"]');
    for(const item of await controls('#site-dialog'))await exercise(item,'site','#site-dialog');
    await check('site-dialog-controls');await button('#site-dialog','Cancel');
    for(const item of await controls('#local-radar-nodes'))await exercise(item,'nodes','#local-radar-nodes');
    await check('node-form-controls');
    report.controlInventory.visited=[...visited].map(([control,result])=>({control,result}));
    report.controlInventory.unavailable=[...unavailable].filter(([key])=>!visited.has(key)).map(([control,reason])=>({control,reason}));
    report.controlInventory.unvisited=[...observed.keys()].filter(key=>!visited.has(key)&&
      unavailable.get(key)!=='Only one enabled choice; no alternative value exists');
    assert.ok(visited.size>80,'Control inventory must cover the full shared and receiver-specific editor');
    assert.deepEqual(report.controlInventory.unvisited,[],'Every visible editable control must be visited or have an explicit unavailable reason');
    console.log(`${visited.size} editable controls exercised; ${report.controlInventory.unavailable.length} unavailable controls recorded.`);
  };
  report.interactions=[];
  report.reviewControls=[];
  const receiverMatrix=async()=>{
  // Each adapter and OS capability is editable on desktop and narrow screens.
  for(const platform of ['linux','darwin'])for(const type of ['Kraken','RspDuo','Usrp','HackRF'])for(const width of [390,1440]){
    await start(type,platform,width);await tab('capture');
    assert.equal(await evaluate(`${query('[data-path="capture.device.type"] select')}.value`),type);
    await reveal(field('capture.fc'));await text('[aria-label="Center frequency Hz"]','1');
    await wait(`!${query('#config-save-later')}.disabled`);await click('#config-save-later');
    await wait(`${query('#config-message')}.textContent.includes('Settings saved')`);
    assert.equal(fixture.state.config.capture.fc,650000001,'MHz input must preserve exact integer Hz');
    const writes=fixture.state.writes.length;
    await text(field('capture.fc'),'-1');await wait(`${query('[data-path="capture.fc"] .config-field-error')}?.textContent.length>0&&${query('#config-save')}.disabled`);
    assert.equal(fixture.state.writes.length,writes,'Invalid edit cannot write settings');
    await check(`${platform}-${type}-${width}-invalid`);await click('#config-reset');
    await wait(`${query(field('capture.fc'))}?.value==='650'`);
    await navigate(route);await tab('capture');
    assert.equal(await evaluate(`${query('[aria-label="Center frequency Hz"]')}.value`),'1','Save survives reload');
  }
  // Receiver switches use actual profile controls and keep every tab reachable.
  await start('Usrp','linux',390);await tab('capture');
  for(const type of ['RspDuo','Kraken','HackRF','Usrp']){
    await select('[data-path="capture.device.type"] select',type);
    assert.equal(await evaluate(`${query('[data-path="capture.device.type"] select')}.value`),type);
  }
  await check('receiver-switches');
  };
  for(const width of [390,1440]){
    await start('Usrp','linux',width,{channelized:true});
    if(width===1440)await navigate(route+'?tab=capture');
    await tab('process');
    await reveal('[data-lane-field="standard"]');
    const lane=key=>`[data-lane-field="${key}"]`;
    for(const standard of ['ATSC-1.0','ATSC-3.0','DVB-T','DVB-T2','CUSTOM']){
      const before=await evaluate(`${query(lane('occupied_bandwidth_hz'))}.value`);
      await select(lane('standard'),standard);
      assert.equal(await evaluate('document.activeElement.dataset.laneField'),'standard','Editor refresh keeps keyboard focus');
      assert.equal(await evaluate(`${query(lane('occupied_bandwidth_hz'))}.value`),before,'Standard must not silently mutate width');
      assert.equal(await evaluate(`${query(lane('atsc_reference_mode'))}.querySelector('[value="pilot_aided"]').disabled`),standard!=='ATSC-1.0');
      if(standard==='ATSC-1.0')await select(lane('atsc_reference_mode'),'pilot_aided');
      if(standard==='ATSC-3.0'){
        assert.equal(await evaluate(`${query(lane('atsc_reference_mode'))}.value`),'pilot_aided','Incompatible saved choice is visible, not silently changed');
        await select(lane('atsc_reference_mode'),'raw');
      }
    }
    await select(lane('reference_channel'),'1');
    for(const widthKhz of ['6000','7000','7600','5380.001']){
      await text(lane('occupied_bandwidth_hz'),widthKhz);await wait(`!${query('#config-save-later')}.disabled`);
    }
    await text(lane('output_sample_rate_hz'),'7000');
    await wait(`${query('.channelizer-preview')}.textContent.includes('divide')`);
    await text(lane('output_sample_rate_hz'),'8000');await text(lane('occupied_bandwidth_hz'),'7000');
    await click('.station-import > summary');
    await text('[aria-label="Station provider JSON"]','{');await button('.station-import','Preview imported stations');
    assert.equal(await evaluate(`[...document.querySelectorAll('.station-import button')].find(e=>e.textContent==='Confirm station changes').disabled`),true);
    const station={schema:1,id:'test-station',provider:{id:'browser-fixture',region:'EX',
      source:'https://example.invalid/stations',retrieved_at:'2026-11-23T00:00:00Z',effective_at:'2026-11-22T00:00:00Z'},
      callsign:'TEST-TV',city:'Example City',state:'Example State',rf_channel:'27',frequency_hz:650000001,
      channel_bandwidth_hz:8000000,occupied_bandwidth_hz:7000001,waveform:{standard:'ATSC',version:'3.0'},
      transmitter:{name:'Imported fixture',latitude:1.2,longitude:2.1,altitude:100},
      provenance:Object.fromEntries(['frequency_hz','channel_bandwidth_hz','occupied_bandwidth_hz','waveform','coordinates'].map(key=>[key,'fixture:'+key]))};
    const importRows=[station,{...clone(station),id:'second-station',callsign:'TEST-TV2',frequency_hz:650000002}];
    if(width===390){
      const filename=require('node:path').join(profile,'station.json');
      require('node:fs').writeFileSync(filename,JSON.stringify(importRows));
      await click('[aria-label="Station provider JSON"]');
      await client.send('Input.dispatchKeyEvent',{type:'keyDown',key:'Tab',code:'Tab',windowsVirtualKeyCode:9});
      await client.send('Input.dispatchKeyEvent',{type:'keyUp',key:'Tab',code:'Tab',windowsVirtualKeyCode:9});
      assert.equal(await evaluate('document.activeElement.getAttribute("aria-label")'),'Import station JSON file');
      const {root}=await client.send('DOM.getDocument');
      const {nodeId}=await client.send('DOM.querySelector',{nodeId:root.nodeId,selector:'[aria-label="Import station JSON file"]'});
      await client.send('DOM.setFileInputFiles',{nodeId,files:[filename]});
      await wait(`!${query('[aria-label="Imported station"]')}.hidden`);
    }else{
      await text('[aria-label="Station provider JSON"]',JSON.stringify(importRows));await button('.station-import','Preview imported stations');
    }
    assert.equal(await evaluate(`${query(lane('frequency_hz'))}.value`),'650','Preview does not apply tuning');
    await text('[aria-label="Search imported stations"]','missing');
    await wait(`${query('.station-import [role="status"]')}.textContent.includes('No matching')`);
    await text('[aria-label="Search imported stations"]','TEST-TV');
    await select('[aria-label="Imported station"]','1');
    assert.match(await evaluate(`${query('.station-import [role="status"]')}.textContent`),/650\.000002/);
    await select('[aria-label="Imported station"]','0');
    const reviewed=index=>`.station-import > div > label:nth-of-type(${index}) input`;
    await text(reviewed(1),'7000.002');
    assert.equal(await evaluate(`[...document.querySelectorAll('.station-import button')].find(e=>e.textContent==='Confirm station changes').disabled`),true,'An override requires a reason');
    await text(reviewed(2),'101');await text(reviewed(3),'Checked the imported metadata');
    await text(reviewed(1),'7000.001');await text(reviewed(2),'100');await text(reviewed(3),'');
    await select('[aria-label="Imported station reference conditioning"]','automatic');
    await select('[aria-label="Imported station reference conditioning"]','raw');
    report.reviewControls=['catalog:station-choice','catalog:reviewed-bandwidth','catalog:verified-altitude',
      'catalog:override-reason','catalog:reference-conditioning'];
    await check(`catalog-preview-${width}`);await button('.station-import','Confirm station changes');
    assert.equal(await evaluate(`${query(lane('frequency_hz'))}.value`),'650.000001');
    assert.equal(await evaluate(`${query(lane('occupied_bandwidth_hz'))}.value`),'7000.001');
    await wait(`!${query('#config-save-later')}.disabled`);await click('#config-save-later');
    await wait(`${query('#config-message')}.textContent.includes('Settings saved')`);
    assert.equal(fixture.state.config.capture.fc,650000000,'Station import cannot retune capture');
    assert.equal(fixture.state.config.process.multi_illuminator.illuminators[0].standard,'ATSC-3.0');
    assert.equal(fixture.state.config.process.multi_illuminator.illuminators[0].reference_channel,1);
    await check(`channelized-save-${width}`);
    await button('.illuminator-list','Add illuminator');
    await wait(`${query('#config-message')}.textContent.includes('overlap')&&${query('#config-save')}.disabled`);
    await click('.illuminator-card:nth-of-type(2) .illuminator-remove');
    await wait('document.querySelectorAll(".illuminator-card").length===1');
    await reveal(field('process.multi_illuminator.enabled'));await click('[data-path="process.multi_illuminator.enabled"] .switch');
    await wait(`!${query('#config-save-later')}.disabled`);await click('#config-save-later');
    await wait(`${query('#config-message')}.textContent.includes('Settings saved')`);
    assert.equal(fixture.state.config.process.multi_illuminator.enabled,false,'Direct mode can be restored');
  }
  await controlInventory(true);
  await receiverMatrix();
  for(const width of [390,1440]){
  // Site edits and hardware-rate refusals have a visible safe recovery path.
  await start('Usrp','linux',width);await tab('capture');await reveal(field('capture.fs'));
  await text(field('capture.fs'),'30.720001');await wait(`${query('[data-path="capture.fs"] .config-field-error')}?.textContent.length>0`);
  await text(field('capture.fs'),'24');await click('[data-site-action="edit"][data-site-role="rx"]');
  await text('#site-input-latitude','91');await click('#site-dialog [type="submit"]');
  assert.equal(await evaluate(`${query('#site-input-latitude')}.checkValidity()`),false);
  assert.equal(await evaluate(`${query('#site-dialog')}.open`),true,'Invalid location stays open');
  await text('#site-input-latitude','1.5');await click('#site-dialog [type="submit"]');
  await wait('!document.querySelector("#site-dialog")');await wait(`!${query('#config-save-later')}.disabled`);
  await click('#config-save-later');await wait(`${query('#config-message')}.textContent.includes('Settings saved')`);
  assert.equal(fixture.state.config.location.rx.latitude,1.5);
  await click('[data-site-action="add"][data-site-role="tx"]');await button('#site-dialog','Cancel');
  await check(`site-and-rate-recovery-${width}`);
  // Wizard validation, error recovery and cancellation use real modal focus.
  await click('[data-start-setup]');await click('[data-wizard-step="1"]');
  await text('[data-wizard-fc]','0');await click('[data-wizard-next]');
  await wait(`${query('.setup-wizard-error')}?.textContent.includes('frequency')`);
  await text('[data-wizard-fc]','650.000001');await click('[data-wizard-next]');
  await wait(query('[data-site="rx.name"]'));await click('[data-wizard-back]');await wait(query('[data-wizard-fc]'));
  await click('[data-wizard-close]');assert.equal(await evaluate('document.querySelectorAll("dialog[open]").length'),0);
  await click('[data-start-setup]');await click('[data-wizard-step="5"]');fixture.state.failSave=true;
  await click('[data-wizard-next]');await wait(`${query('.setup-wizard-error')}?.textContent.includes('Reconnect the USB cable')`);
  assert.doesNotMatch(await evaluate(`${query('.setup-wizard-error')}.textContent`),/Send report/,'Modal copies the error, not unrelated actions');
  assert.equal(await evaluate(`(()=>{const e=${query('.setup-wizard-error')},r=e.getBoundingClientRect(),footer=document.querySelector('.setup-wizard-content > footer').getBoundingClientRect();
    return document.activeElement===e&&r.top>=0&&r.bottom<=Math.min(innerHeight,footer.top)+2;})()`),true,'Startup recovery instruction is visible above the modal footer and focused');
  await check(`wizard-start-failure-${width}`);fixture.state.failSave=false;await click('[data-wizard-next]');
  await wait('!document.querySelector(".setup-wizard[open]")');await check(`wizard-recovered-${width}`);
  await start('Usrp','darwin',width,{unavailable:true});await click('[data-start-setup]');
  await click('[data-wizard-step="0"]');
  await click('[data-wizard-receiver-software]');await wait('!document.querySelector(".setup-wizard[open]")');
  assert.equal(await evaluate('document.querySelector(".config-tabs [aria-selected=true]")?.dataset.tab'),'capture');
  await check(`missing-receiver-software-${width}`);
  }
  for(const width of [390,1440]){
    await start('Usrp','linux',width,{migration:true,review:true});await wait(query('#config-migration-dialog[open]'));
    assert.equal(await evaluate('document.activeElement.closest("dialog")?.id'),'config-migration-dialog');
    assert.equal(await evaluate(`(()=>{const d=${query('#config-migration-dialog')},t=${query('#config-migration-title')},r=t.getBoundingClientRect();
      return document.activeElement===t&&d.scrollTop===0&&r.top>=0&&r.bottom<=innerHeight;})()`),true,
    'Upgrade notice opens with its title visible and focused, not scrolled to acknowledgement');
    assert.match(await evaluate(`${query('#config-migration-dialog')}.textContent`),/Do not repeat receiver setup/);
    await check(`migration-review-${width}`);
    const mark=label=>{if(width===1440)console.log(`Node recovery diagnostic: ${label}`);};
    mark('migration review recorded');
    if(width===390)await popup('#config-migration-dialog a');
    else {
      mark('receipt inspection start');
      const receiptLink=await evaluate(`(()=>{const e=${query('#config-migration-dialog a')};return {href:e.href,target:e.target,rel:e.rel};})()`);
      mark('receipt inspection complete');
      assert.equal(new URL(receiptLink.href).origin,await evaluate('location.origin'),'Desktop receipt link stays fixture-local');
      mark('receipt origin complete');
      assert.equal(receiptLink.target,'_blank','Desktop receipt link preserves a separate review surface');
      assert.match(receiptLink.rel,/noreferrer/,'Desktop receipt link isolates the review surface');
    }
    mark('acknowledgement click start');
    await click('#config-migration-dialog button');
    mark('acknowledgement click complete');
    await navigate(route);assert.equal(await evaluate('!!document.querySelector("#config-migration-dialog")'),false,'Acknowledgement survives reload');
    mark('acknowledgement reload complete');
    await click('#config-migration-history');await wait(query('#config-migration-dialog[open]'));
    mark('history dialog open');
    await client.send('Input.dispatchKeyEvent',{type:'keyDown',key:'Escape',code:'Escape',windowsVirtualKeyCode:27});
    await client.send('Input.dispatchKeyEvent',{type:'keyUp',key:'Escape',code:'Escape',windowsVirtualKeyCode:27});
    await wait('!document.querySelector("#config-migration-dialog")');assert.equal(fixture.state.writes.length,0,'Acknowledgement never writes settings');
    mark('history dialog closed');
    await text('#local-radar-nodes [name="host"]','bad');await click('#local-radar-nodes button[type="submit"]');
    await wait(`${query('.local-node-message')}.textContent.includes('IP address')`);
    mark('invalid node complete');
    await text('#local-radar-nodes [name="host"]','192.0.2.10');fixture.state.failNode=true;
    await click('#local-radar-nodes button[type="submit"]');await wait(`${query('.local-node-message')}.textContent.includes('try again')`);
    mark('failed node complete');
    fixture.state.failNode=false;await click('#local-radar-nodes button[type="submit"]');
    await wait(query('.local-node-list button'));assert.equal(fixture.state.nodes.length,1);
    mark('node added');
    let accept=false,dialogClosed;
    client.on('Page.javascriptDialogOpening',()=>client.send('Page.handleJavaScriptDialog',{accept}).catch(()=>{}));
    client.on('Page.javascriptDialogClosed',()=>{dialogClosed?.();dialogClosed=null;});
    const clickRemoval=async()=>{
      const closed=new Promise((resolve,reject)=>{const timeout=setTimeout(()=>reject(new Error('Node confirmation did not close')),10000);
        dialogClosed=()=>{clearTimeout(timeout);resolve();};});
      await click('.local-node-list button');await closed;
    };
    await clickRemoval();assert.equal(fixture.state.nodes.length,1,'Cancel preserves peer');
    mark('cancel removal complete');
    accept=true;await clickRemoval();await wait(`${query('.local-node-message')}.textContent.includes('Node removed')`);
    mark('accepted removal complete');
    assert.equal(fixture.state.nodes.length,0);
    await wait(`!${query('.local-node-list button')}&&!validationPending&&!upstreamPending`);
    mark('node state settled');
    const nodeRecovery=await evaluate(`(()=>{const panel=${query('#local-radar-nodes')},message=${query('.local-node-message')};
      const r=panel.getBoundingClientRect();return {buttons:panel.querySelectorAll('.local-node-list button').length,
        message:message.textContent.trim(),contained:r.left>=-2&&r.right<=innerWidth+2,
        overflow:panel.scrollWidth-panel.clientWidth};})()`);
    mark('node layout inspected');
    assert.equal(nodeRecovery.buttons,0,'Removed peers are absent from the rendered list');
    assert.match(nodeRecovery.message,/Node removed/,'Node removal reports success');
    assert.equal(nodeRecovery.contained,true,`Node panel stays inside the ${width}px viewport`);
    assert.ok(nodeRecovery.overflow<=2,`Node panel has no horizontal overflow at ${width}px`);
    report.checks.push({route,
      state:`interaction:nodes-recovery-${width}`,width,nodeRecovery});
    await screenshot(`interaction-nodes-recovery-${width}`);
    report.interactions.push(`nodes-recovery-${width}`);console.log(`Checked interaction nodes-recovery-${width}`);
  }
  // Header links must navigate through the shipped browser handlers, not just
  // respond to direct URL loads in the route/geometry sweep above.
  for(const width of [390,1440]){
    await start('Usrp','linux',width);
    const links=await evaluate('[...document.querySelectorAll(".app-nav a")].map(e=>e.getAttribute("href"))');
    for(const href of links){
      const loaded=new Promise((resolve,reject)=>{const timeout=setTimeout(()=>reject(new Error(`Navigation timed out: ${href}`)),20000);
        client.on('Page.loadEventFired',()=>{clearTimeout(timeout);resolve();});});
      await Promise.all([click(`.app-nav a[href="${href}"]`),loaded]);
      await wait(`location.pathname===${JSON.stringify(new URL(href,'http://fixture').pathname)}&&document.readyState==='complete'&&document.querySelector('.app-nav [aria-current="page"]')`,20000);
      await check(`navigation-${href.replace(/[^a-z0-9]/gi,'_')}-${width}`);
    }
  }
  await actionCensus();
  await controlInventory();
  finalizeActions();
  console.log(`${report.interactions.length} setup/save/catalog/migration/node interaction checks passed.`);
}
module.exports={createFixture,runInteractions};
