'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM} = require('./jsdom');
const {configurationForReceiver} = require('../../api/config-store');
const configManager = require('../../api/config-manager');
const {getDeviceProfiles, FIELD_RULES} = configManager;
const {presetRecord} = require('../../html/js/kraken_geometry');
const capabilityOwner = require.resolve('../../html/js/processing_capabilities');
assert.ok(require.cache[capabilityOwner],
  'Validator loads the browser capability owner directly');
const browserTopology = require('../../html/js/processing_capabilities');
const profiles = ['Kraken', 'Usrp', 'RspDuo', 'HackRF'];
const clone = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));

function configuration(type, {array = false, replay = false, enabled = true} = {}) {
  const config = configurationForReceiver(type);
  config.process.tracker.gpu_capacity = 256;
  config.process.tracker.initiate.stationary_clutter = {enable:enabled,persistence_cpis:12,max_doppler_hz:5};
  Object.assign(config.process.detection.track_before_detect, {enable:enabled,max_cpis:4,fractional_delay:enabled});
  config.process.tracker.initiate.N = 1 + 4 * (config.process.tracker.initiate.M - 1);
  config.process.tracker.delete = Math.max(config.process.tracker.delete, 3);
  config.process.bearing.method = 'covariance_mvdr';
  if (array) {
    const template = configurationForReceiver('Kraken');
    const geometry = presetRecord(template, 'pentagon5').geometry;
    Object.assign(geometry, {mapping_confirmed:true,geometry_confirmed:true,
      orientation_confirmed:true,x_axis_bearing_deg_true:0});
    config.capture.device.array_geometry = geometry;
    config.process.surveillance_fusion = {...template.process.surveillance_fusion,mode:'coherent_beam_bank'};
    if (replay) config.capture.replay = {...config.capture.replay,state:true,channel_count:5,
      reference_mode:'array_eigenbeam',reference_channel:0,reference_channels:[0,1,2,3,4],surveillance_channels:[0,1,2,3,4]};
    else {
      config.capture.device.reference_channel = 0;
      config.capture.device.surveillance_channels = [0,1,2,3,4];
      config.process.reference_synthesis.mode = 'array_eigenbeam';
      config.process.reference_synthesis.channels = [0,1,2,3,4];
    }
  } else if (type === 'Kraken') {
    config.process.reference_synthesis.mode = 'dedicated';
    config.process.reference_synthesis.channels = [0];
    config.capture.device.reference_channel = 0;
    config.capture.device.surveillance_channels = [1,2,3,4];
  }
  return config;
}

async function fixture(config, selectable, test, recorded = null) {
  const before = JSON.stringify(config);
  const dom = new JSDOM('<div id="configuration"></div>', {url:'http://fixture/display/configuration/' +
    (recorded ? '?replayFile=%2Frecording.iq&replayId=recorded' : ''),runScripts:'outside-only'});
  const w = dom.window;
  w.liveApiUrl = value => value; w.rememberApiPort = () => {}; w.setInterval = () => 0;
  w.fetchStatusResource = (url, options) => w.fetch(url, options);
  w.fetch = async (url, options = {}) => {
    assert.ok(!options.method || options.method === 'GET' || url === '/api/config/validate', 'No configuration or hardware writes');
    const body = url === '/api/recordings/recorded/replay-profile' ?
      {configuration_complete:true,profile:clone(recorded)} : url === '/api/config' ? clone(config) : url === '/api/config/capabilities' ? {
      editable:true,restartAvailable:false,fieldRules:FIELD_RULES,
      // Deliberately deny every static feature: current topology/backend wins.
      deviceProfiles:getDeviceProfiles().map(profile=>({...profile,features:{coherentArray:false,residentIntegration:false,bearing:false}})),
      processingBackends:{stages:{resident:{selectable,qualified:false,selected:false,executed:false,
        reason:selectable?'FP64 resident module selectable':'FP32-only GPU'}}}
    } : url === '/api/config/validate' ? {valid:true,errors:[]} : {};
    return {ok:true,status:200,headers:{get:()=>null},json:async()=>body};
  };
  try {
    for (const file of ['processing_capabilities','config_profiles','kraken_geometry','config_ui'])
      w.eval(fs.readFileSync(path.join(__dirname,'../../html/js',`${file}.js`),'utf8') +
        (file === 'config_ui' ? '\nwindow.readFixtureDraft = () => JSON.stringify(activeConfig);' : ''));
    await w.renderConfiguration();
    const rendered = JSON.parse(w.readFixtureDraft());
    const expected = recorded || config;
    assert.deepEqual(rendered.process.detection.track_before_detect,expected.process.detection.track_before_detect,'Rendering preserves every saved TBD setting');
    assert.equal(rendered.process.tracker.gpu_capacity,expected.process.tracker.gpu_capacity);
    const control = key => w.document.querySelector(`[data-path="${key}"] input, [data-path="${key}"] select`);
    const option = value => w.document.querySelector(`[data-processing-control="recovery-mode"] option[value="${value}"]`);
    const disabled = (key, expected) => {assert.ok(control(key), `Rendered ${key}`);assert.equal(control(key).disabled,expected,key);};
    await test({w,control,option,disabled});
    assert.equal(JSON.stringify(config),before,'Fixture saved values never mutate');
  } finally { w.close(); }
}
const tbd = 'process.detection.track_before_detect';
const stationary = 'process.tracker.initiate.stationary_clutter.enable';
const capacity = 'process.tracker.gpu_capacity';

(async()=>{
  const local = configuration('Kraken', {array:true});
  for (const mode of ['noncoherent','coherent_beam_bank','noise_whitened_beam_bank']) {
    const observed = clone(local); observed.process.performance.acceleration = 'gpu-resident';
    observed.process.surveillance_fusion.mode = mode;
    const before = browserTopology.experimentalSettingSupport(observed);
    observed.capture.replay.resident_snapshot = {enabled:true};
    assert.deepEqual(browserTopology.experimentalSettingSupport(observed),before,
      'Snapshot observation cannot select product ownership or alter supported policies');
  }
  const recorded = clone(local);
  Object.assign(recorded.process.performance, {acceleration:'cpu',surveillance_workers:3,fft_threads:2});
  recorded.process.detection.track_before_detect.motion_bins = 2;
  await fixture(local, true, ({w}) => {
    assert.deepEqual(JSON.parse(w.readFixtureDraft()).process, recorded.process,
      'Replay selection preserves the entire recorded processing plan, including performance');
  }, recorded);
  for (const replay of [false,true]) {
    const canonical = configuration('Kraken',{array:true,replay});
    canonical.process.bearing.enable = true;
    canonical.process.bearing.reference_anchor = 'fixed_site_projected';
    canonical.process.performance.acceleration = 'gpu-resident';
    canonical.process.surveillance_fusion.direct_path_null_db = 0;
    delete canonical.process.bearing.manifold_file;
    await fixture(canonical,true,({w})=>assert.equal(w.document.querySelector(
      '[data-path="process.bearing.reference_anchor"] option[value="fixed_site_projected"]').disabled,false,
      'Explicit projected geometry is transport-independent'));
    canonical.process.performance.acceleration = 'cpu';
    const support = browserTopology.experimentalSettingSupport(canonical);
    assert.equal(support.targetProtection,true,'Configured target protection is transport-independent');
    assert.equal(support.complexCutout,replay,'Complex export remains replay-only');
    await fixture(canonical,true,({w,control})=>{
      const target = control('process.reference_synthesis.target_protection_enabled');
      assert.equal(target.disabled,false);
      target.checked = true; target.dispatchEvent(new w.Event('change',{bubbles:true}));
      for (const key of ['mapping_confirmed','geometry_confirmed','orientation_confirmed'])
        assert.equal(JSON.parse(w.readFixtureDraft()).capture.device.array_geometry[key],true,
          'A processing-only edit preserves physical confirmations');
      const previous = JSON.parse(w.readFixtureDraft()), changed = clone(previous);
      changed.capture.device.array_geometry.elements[0].position[0] += .01;
      w.KrakenGeometry.reconcile(changed,previous);
      assert.equal(changed.capture.device.array_geometry.geometry_confirmed,false,
        'Physical changes still invalidate the browser confirmation');
    });
  }
  for (const type of profiles) {
    const portable = configuration(type,{enabled:false});
    delete portable.process.tracker.beam_tolerance;
    delete portable.process.tracker.gpu_capacity;
    portable.process.performance.acceleration = 'cpu';
    await fixture(portable,false,({w,option})=>{
      const pin=w.document.querySelector('[data-path="capture.replay.recording_sha256"] input');
      assert.ok(pin && !pin.disabled && !pin.required,'Every receiver exposes optional replay trust pins');
      assert.equal(JSON.parse(w.readFixtureDraft()).capture.replay.recording_sha256,undefined,'Rendering does not invent trust');
      pin.value='a'.repeat(64);pin.dispatchEvent(new w.Event('input',{bubbles:true}));
      assert.equal(JSON.parse(w.readFixtureDraft()).capture.replay.recording_sha256,'a'.repeat(64));
      pin.value='';pin.dispatchEvent(new w.Event('input',{bubbles:true}));
      assert.equal(JSON.parse(w.readFixtureDraft()).capture.replay.recording_sha256,undefined,'Clearing a pin removes it');
      const select=option('two_motion').parentElement;select.value='two_motion';
      select.dispatchEvent(new w.Event('change',{bubbles:true}));
      const draft=JSON.parse(w.readFixtureDraft());
      assert.equal(draft.process.detection.integration_cpis,2);
      assert.equal(draft.process.detection.integration_motion_bins,1);
      assert.equal(draft.process.performance.acceleration,'cpu','Portable two-CPI preset keeps the selected CPU backend');
      assert.equal(draft.process.tracker.gpu_capacity,undefined,'Portable integration does not inject resident tuning');
      const hardware=w.document.querySelector('[data-path="process.performance.acceleration"] select');
      hardware.value='automatic';hardware.dispatchEvent(new w.Event('change',{bubbles:true}));
      assert.equal(JSON.parse(w.readFixtureDraft()).process.performance.acceleration,'auto','Automatic does not turn portable integration into resident processing');
      const cpu=w.document.querySelector('[data-path="process.performance.acceleration"] select');
      cpu.value='cpu';cpu.dispatchEvent(new w.Event('change',{bubbles:true}));
      const cpuDraft=JSON.parse(w.readFixtureDraft());
      assert.equal(cpuDraft.process.detection.integration_cpis,2,'CPU-only preserves portable two-CPI integration');
      assert.equal(cpuDraft.process.detection.integration_motion_bins,1);
    });
    await fixture(configuration(type,{array:type==='Kraken'}),false,({w,disabled,control,option})=>{
      for (const key of [`${tbd}.enable`,`${tbd}.fractional_delay`,`${tbd}.max_cpis`,stationary]) disabled(key,false);
      disabled(capacity,true);
      assert.equal(control(`${tbd}.enable`).checked,true,'Unavailable saved enable retained');
      assert.equal(control(capacity).value,'256');
      for (const mode of ['two','two_motion']) assert.equal(option(mode).disabled,false);
      for (const mode of ['four','eight']) assert.equal(option(mode).disabled,false);
      assert.equal(option('four').parentElement.value,'four','Saved power preset stays selected');
      assert.equal(w.document.querySelectorAll('#processing-capability-note').length,1);
      assert.match(w.document.getElementById('processing-capability-note').textContent,/FP32-only GPU.*Saved values are preserved/);
      const whiten=w.document.querySelector('[data-path="process.surveillance_fusion.mode"] option[value="noise_whitened_beam_bank"]');
      if(whiten)assert.equal(whiten.disabled,true);
    });
    await fixture(configuration(type),true,({disabled,option})=>{
      disabled(capacity,false);disabled(stationary,false);
      for (const key of [`${tbd}.enable`,`${tbd}.fractional_delay`,`${tbd}.motion_bins`]) disabled(key,false);
      for(const mode of ['two','two_motion'])assert.equal(option(mode).disabled,false);
      for(const mode of ['four','eight'])assert.equal(option(mode).disabled,false);
    });
  }
  await fixture(configuration('Kraken',{array:true}),true,({w,disabled,option})=>{
    for(const key of [`${tbd}.enable`,`${tbd}.fractional_delay`,stationary,capacity])disabled(key,false);
    for(const mode of ['two','two_motion','four','eight'])assert.equal(option(mode).disabled,false);
    assert.equal(w.document.querySelector('[data-path="process.surveillance_fusion.mode"] option[value="noise_whitened_beam_bank"]').disabled,false);
    const select=option('two').parentElement;select.value='two';select.dispatchEvent(new w.Event('change',{bubbles:true}));
    const draft=JSON.parse(w.readFixtureDraft());
    assert.equal(draft.process.detection.track_before_detect.fractional_delay,false);
  });
  const noFusion=configuration('Kraken',{array:true});noFusion.process.surveillance_fusion.mode='noncoherent';
  await fixture(noFusion,true,({disabled,option})=>{disabled(`${tbd}.enable`,false);assert.equal(option('four').disabled,false);});
  const beamConditioned=configuration('Kraken',{array:true});beamConditioned.process.tracker.beam_tolerance=1;
  await fixture(beamConditioned,false,({disabled,option,control})=>{
    disabled('process.detection.integration_cpis',true);
    assert.equal(option('two').disabled,true);
    assert.match(control('process.detection.integration_cpis').title,/Beam-conditioned/);
  });
  const irregular=configuration('Kraken',{array:true});irregular.capture.device.array_geometry.elements[0].position[0]+=.1;
  await fixture(irregular,true,({disabled})=>{disabled(`${tbd}.enable`,false);disabled('process.bearing.enable',true);});
  const unconfirmed=configuration('Kraken',{array:true});unconfirmed.capture.device.array_geometry.mapping_confirmed=false;
  await fixture(unconfirmed,true,({disabled})=>{disabled(`${tbd}.enable`,false);disabled('process.bearing.enable',true);});
  for (const feature of ['clutter','detection','tracker']) {
    const disabledFeature=configuration('Kraken',{array:true});disabledFeature.process[feature].enable=false;
    await fixture(disabledFeature,true,({disabled})=>{
      disabled(stationary,feature!=='clutter');disabled(capacity,true);
      disabled(`${tbd}.enable`,feature==='detection');
      disabled('process.detection.integration_cpis',feature==='detection');
    });
  }
  const invalidRoles=configuration('Usrp');invalidRoles.process.reference_synthesis={mode:'dedicated',channels:[0,0]};
  await fixture(invalidRoles,true,({option,disabled})=>{assert.equal(option('two').disabled,true);disabled(capacity,true);});
  for(const type of profiles) {
    const config=configuration(type,{array:true,replay:true,enabled:false});
    const replay=clone(config.capture.replay),device=clone(config.capture.device),reference=clone(config.process.reference_synthesis);
    await fixture(config,true,({w,control,disabled,option})=>{
      assert.ok(w.document.querySelector('[role="tab"][data-tab="array"]'),'Recorded array has an Array tab');
      assert.match(w.document.querySelector('.kraken-geometry summary').textContent,/Recorded array/);
      assert.equal(w.document.querySelectorAll('.geometry-element').length,5);
      assert.equal(w.document.querySelectorAll('[data-path^="capture.device.heimdall"],[data-path="capture.device.type"],[data-path="capture.device.serial"],[data-path="capture.device.address"],[data-path="capture.fc"],[data-path="capture.fs"],.geometry-channel-editor,.geometry-preset-counts,#upstream-status,#receiver-startup,#receiver-health').length,0,'No dormant live receiver/tuning/role controls during replay');
      const workers = control('process.performance.surveillance_workers');
      assert.equal(workers.options.length,6,'Recorded five-channel array exposes Auto and all five worker counts');
      assert.ok(control('capture.device.array_geometry.elements.0.position.0'));
      assert.equal(control('capture.device.array_geometry.elements.0.position.0').disabled,false);
      disabled(`${tbd}.enable`,false);disabled(`${tbd}.fractional_delay`,true);
      assert.equal(option('four').disabled,false);
      const select=option('four').parentElement;select.value='four';select.dispatchEvent(new w.Event('change',{bubbles:true}));
      disabled(`${tbd}.fractional_delay`,false);
      assert.equal(control(`${tbd}.enable`).checked,true);
      const draft=JSON.parse(w.readFixtureDraft());
      assert.deepEqual(draft.capture.replay,replay,'Recovery preset retains recorded input metadata');
      assert.deepEqual(draft.capture.device,device,'Recovery preset does not rewrite live receiver settings or confirmed geometry');
      assert.deepEqual(draft.process.reference_synthesis,reference,'Recovery preset retains dormant live reference mapping');
      assert.equal(draft.process.performance.acceleration,config.process.performance.acceleration,
        'Power recovery preserves the chosen processing backend');
    });
  }
  const noTruth=configuration('Kraken',{array:true});noTruth.truth.adsb.enabled=false;
  await fixture(noTruth,true,({disabled})=>{
    disabled('process.bearing.manifold_recorder.enabled',true);
  });
  const badTiming=configuration('Usrp');badTiming.capture.fs=0;
  await fixture(badTiming,false,({disabled,control})=>{
    disabled(`${tbd}.enable`,false);disabled(`${tbd}.fractional_delay`,true);
    assert.match(control(`${tbd}.fractional_delay`).title,/sample rate/);
  });
  const multi=configuration('HackRF');multi.process.multi_illuminator={enabled:true,
    illuminators:[{id:'one',enabled:true},{id:'two',enabled:true}]};
  await fixture(multi,false,({disabled,option})=>{
    for(const key of [`${tbd}.enable`,stationary])disabled(key,false);
    assert.equal(option('eight').disabled,false);
  });
  for (const selectable of [false,true]) {
    const coherent=configuration('Usrp',{array:true,replay:true});
    coherent.capture.replay.format='blah2';coherent.process.bearing.enable=true;
    coherent.process.bearing.reference_anchor='fixed_site_projected';
    coherent.process.performance.acceleration=selectable?'gpu-resident':'cpu';
    coherent.process.multi_illuminator={enabled:true,illuminators:[
      {id:'one',enabled:true,frequency_hz:90000000,output_sample_rate_hz:300000,
        transmitter:{name:'Display site',latitude:40,longitude:-80,altitude:300}},
      {id:'two',enabled:true,frequency_hz:91000000,output_sample_rate_hz:300000,
        transmitter_location:{latitude:41,longitude:-81,altitude:310}}]};
    await fixture(coherent,selectable,({w,disabled})=>{
      const fusion=w.document.querySelector('[data-path="process.surveillance_fusion.mode"] option[value="coherent_beam_bank"]');
      assert.equal(fusion.disabled,true,'Legacy transmitter metadata is not an implicit coherent site');
      assert.match(fusion.title,/explicit transmitter/);
      disabled('process.bearing.enable',true);
      const card=w.document.querySelector('.illuminator-card');
      const get=key=>card.querySelector(`[data-lane-field="${key}"]`);
      assert.equal(get('transmitter_location.latitude').value,'','Missing geometry remains missing');
      assert.equal(JSON.parse(w.readFixtureDraft()).process.multi_illuminator.illuminators[0].transmitter_location,undefined);
      assert.match(card.querySelector('.illuminator-calibration-note').textContent,/RF frequency.*output rate/);
      for(const [key,value] of Object.entries({latitude:40.5,longitude:-80.5,altitude:320})) {
        const input=get(`transmitter_location.${key}`);input.value=String(value);
        input.dispatchEvent(new w.Event('input',{bubbles:true}));
      }
      assert.equal(fusion.disabled,false,'Explicit lane sites unlock qualified coherent fusion on CPU and resident');
      disabled('process.bearing.enable',!selectable);
      disabled(capacity,!selectable);
      disabled('process.bearing.manifold_recorder.enabled',true);
      const projected=w.document.querySelector('[data-path="process.bearing.reference_anchor"] option[value="fixed_site_projected"]');
      assert.equal(projected.disabled,!selectable,'Ideal qualified coherent replay lanes admit projected bearing');
      assert.deepEqual(JSON.parse(w.readFixtureDraft()).process.multi_illuminator.illuminators[0].transmitter_location,
        {latitude:40.5,longitude:-80.5,altitude:320});
      const calibration=get('detection_manifold_file');calibration.value='/recording/lane-one-calibration.json';
      calibration.dispatchEvent(new w.Event('input',{bubbles:true}));
      assert.equal(JSON.parse(w.readFixtureDraft()).process.multi_illuminator.illuminators[0].detection_manifold_file,
        '/recording/lane-one-calibration.json');
      assert.equal(projected.disabled,true,'Measured lane calibration cannot use ideal projected bearing');
      assert.match(projected.title,/no measured manifold/);
    });
    coherent.process.multi_illuminator.illuminators[0].transmitter_location={latitude:40,longitude:-80,altitude:300};
    coherent.process.multi_illuminator.illuminators[0].reference_channel=0;
    coherent.process.multi_illuminator.illuminators[0].surveillance_channels=[1,2,3,4];
    await fixture(coherent,selectable,({w,disabled})=>{
      const fusion=w.document.querySelector('[data-path="process.surveillance_fusion.mode"] option[value="coherent_beam_bank"]');
      assert.equal(fusion.disabled,true);assert.match(fusion.title,/dedicated per-lane overrides/);
      disabled('process.bearing.enable',true);
      assert.equal(JSON.parse(w.readFixtureDraft()).process.multi_illuminator.illuminators[0].reference_channel,0,
        'Unsupported role override is preserved until an explicit operator edit');
    });
  }
  console.log('Processing capability DOM matrix passed: four FP32 profiles, FP64 dedicated profiles, coherent array, prerequisites, and recorded-array replay under all live profiles.');
})().catch(error=>{console.error(error);process.exitCode=1;});
