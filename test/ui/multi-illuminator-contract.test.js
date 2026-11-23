'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const path = require('path');

function fixture(search = '', stored = 'wesa', enabled = true, catalog = null) {
  const storage = new Map([['vectorwarp-selected-illuminator-v1:local', stored]]);
  const recording = {status: {innerHTML: ''}, copy: {textContent: ''},
    button: {disabled: false, textContent: ''}, pause: {hidden: false, textContent: ''}};
  const recordingControl = {classList: {toggle() {}}, querySelector(selector) {
    return {'.recording-status': recording.status, '.recording-copy': recording.copy,
      '.recording-toggle': recording.button, '.recording-pause': recording.pause}[selector] || null;
  }};
  const context = vm.createContext({
    URL, URLSearchParams, AbortController,
    document: {body: {dataset: {insightView: 'tracks'}, classList: {toggle() {}}}, hidden: false,
      querySelector: () => null, querySelectorAll: () => [],
      getElementById: id => id === 'recording-control' ? recordingControl : null},
    window: {location: new URL(`http://vectorwarp.test/display/tracks/${search}`),
      localStorage: {getItem: key => storage.get(key) || null,
        setItem: (key, value) => storage.set(key, value)},
      sessionStorage: {getItem: () => null, setItem() {}}, setTimeout, clearTimeout},
    fetch: async url => {
      const pathname = new URL(url, 'http://vectorwarp.test').pathname;
      if (pathname === '/api/system/status')
        return {ok: true, status: 200, text: async () => JSON.stringify({serverId: 'fixture', radar: 'receiving'})};
      if (pathname === '/api/runtime/config')
        return {ok: true, status: 200, text: async () => JSON.stringify({capture: {
          fc: 101100000, fs: 2400000}})};
      assert.equal(pathname, '/api/illuminators/live');
      return {ok: true, status: 200, text: async () => JSON.stringify(catalog || {enabled,
        primary: 'wqed', illuminators: [
          {id: 'wqed', frequency_hz: 89300000, output_sample_rate_hz: 300000},
          {id: 'wesa', frequency_hz: 90500000, output_sample_rate_hz: 320000},
          {id: 'wyep', frequency_hz: 91300000, output_sample_rate_hz: 280000}
        ]})};
    }
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../../html/js/common.js'), 'utf8'), context);
  return {context, storage, recording};
}

(async () => {
  const saved = fixture();
  const selected = await saved.context.getIlluminatorContext();
  assert.equal(selected.id, 'wesa', 'the persistent selection wins over the primary default');
  assert.equal(saved.context.selectedIlluminatorUrl('/api/tracker?presentation=aligned', selected),
    '/api/tracker?presentation=aligned&illuminator=wesa');
  const effective = saved.context.selectedIlluminatorConfig({capture: {fs: 2400000},
    location: {rx: {name: 'RX'}, tx: {name: 'legacy'}}, process: {multi_illuminator: {illuminators: [
      {id: 'wesa', output_sample_rate_hz: 320000,
        transmitter: {name: 'WESA', latitude: 40.4, longitude: -79.9}}
    ]}}}, selected);
  assert.equal(effective.capture.fs, 320000);
  assert.equal(effective.location.tx.name, 'WESA');
  const coherentConfig={capture:{fs:2400000},location:{tx:{latitude:1,longitude:2,altitude:3}},
    process:{surveillance_fusion:{mode:'coherent_beam_bank'},multi_illuminator:{enabled:true,illuminators:[
      {id:'wesa',transmitter:{name:'WESA',latitude:40,longitude:-80,altitude:200},
        transmitter_location:{latitude:41,longitude:-81,altitude:300}}]}}};
  const coherent=saved.context.selectedIlluminatorConfig(coherentConfig,selected);
  assert.equal(coherent.location.tx.name,'WESA');
  assert.equal(coherent.location.tx.latitude,41,'Selected display geometry matches the native lane site');
  delete coherentConfig.process.multi_illuminator.illuminators[0].transmitter_location;
  assert.equal(saved.context.selectedIlluminatorConfig(coherentConfig,selected).location.tx.latitude,undefined,
    'Missing coherent site cannot inherit global or legacy display coordinates');
  await saved.context.initializeIlluminatorDisplay();
  assert.match(saved.recording.copy.textContent, /Full-passband recording includes every illuminator lane/,
    'recording copy refreshes after the asynchronous lane catalog resolves');

  const explicit = fixture('?illuminator=wyep');
  assert.equal((await explicit.context.getIlluminatorContext()).id, 'wyep',
    'an explicit URL selection wins over persistent and primary defaults');
  assert.equal(explicit.storage.get('vectorwarp-selected-illuminator-v1:local'), 'wyep');

  const disabled = fixture('', 'wesa', false);
  const primary = await disabled.context.getIlluminatorContext();
  assert.equal(primary.id, 'primary');
  assert.equal(primary.item.api_illuminator, false,
    'retained lane definitions are ignored while multi-illuminator processing is disabled');
  assert.equal(disabled.context.selectedIlluminatorUrl('/api/radar/frame', primary),
    '/api/radar/frame');
  const nativePrimary = fixture('', '', true, {enabled: true, primary: 'primary',
    illuminators: [{id: 'primary', api_illuminator: false, available: false,
      frequency_hz: 509000000, output_sample_rate_hz: 2400000}]});
  const reported = await nativePrimary.context.getIlluminatorContext();
  assert.equal(reported.item.frequency_hz, 509000000);
  assert.equal(reported.item.available, false, 'Catalog freshness must not be replaced with an invented true value');
  assert.equal(nativePrimary.context.selectedIlluminatorUrl('/api/tracker', reported), '/api/tracker');

  const settingsSource=fs.readFileSync(path.join(__dirname,'../../html/js/config_ui.js'),'utf8');
  assert.doesNotMatch(settingsSource,/function multiIlluminatorUnsupportedPath|function markMultiIlluminatorUnsupported/,
    'Multi-lane availability is determined by topology, explicit sites and backend capabilities, not a blanket gate');
  console.log('Persistent multi-illuminator selection, lane geometry/rate, and capability-driven settings contract passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
