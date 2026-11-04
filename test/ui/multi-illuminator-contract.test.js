'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const {multiIlluminatorUnsupportedPath} = require('../../html/js/config_ui.js');

function fixture(search = '', stored = 'wesa') {
  const storage = new Map([['vectorwarp-selected-illuminator-v1', stored]]);
  const recording = {status: {innerHTML: ''}, copy: {textContent: ''},
    button: {disabled: false, textContent: ''}, pause: {hidden: false, textContent: ''}};
  const recordingControl = {classList: {toggle() {}}, querySelector(selector) {
    return {'.recording-status': recording.status, '.recording-copy': recording.copy,
      '.recording-toggle': recording.button, '.recording-pause': recording.pause}[selector] || null;
  }};
  const context = vm.createContext({
    URL, URLSearchParams, AbortController,
    document: {body: {dataset: {insightView: 'tracks'}}, hidden: false,
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
      assert.equal(pathname, '/api/illuminators/live');
      return {ok: true, status: 200, text: async () => JSON.stringify({enabled: true,
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
  await saved.context.initializeIlluminatorDisplay();
  assert.match(saved.recording.copy.textContent, /Full-passband recording includes every illuminator lane/,
    'recording copy refreshes after the asynchronous lane catalog resolves');

  const explicit = fixture('?illuminator=wyep');
  assert.equal((await explicit.context.getIlluminatorContext()).id, 'wyep',
    'an explicit URL selection wins over persistent and primary defaults');
  assert.equal(explicit.storage.get('vectorwarp-selected-illuminator-v1'), 'wyep');

  for (const path of ['process.surveillance_fusion.mode',
    'process.detection.integration_cpis', 'process.detection.track_before_detect.max_cpis',
    'process.tracker.beam_tolerance', 'process.tracker.gpu_capacity',
    'process.tracker.initiate.stationary_clutter.persistence_cpis',
    'process.tracker.track_guided_shadow.max_tracks', 'process.bearing.manifold_file'])
    assert.equal(multiIlluminatorUnsupportedPath(path), true, path);
  assert.equal(multiIlluminatorUnsupportedPath('process.tracker.delete'), false);
  assert.equal(multiIlluminatorUnsupportedPath(
    'process.tracker.initiate.acceleration_model'), false);
  assert.equal(multiIlluminatorUnsupportedPath('process.detection.pfa'), false);
  console.log('Persistent multi-illuminator selection, lane geometry/rate, and disabled settings contract passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
