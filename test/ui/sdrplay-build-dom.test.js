'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('jsdom');
const {setupDefaults} = require('../../api/config-store');
const {getDeviceProfiles, FIELD_RULES, validateConfig} = require('../../api/config-manager');

const dom = new JSDOM('<div id="configuration"></div>', {url: 'http://127.0.0.1:3000/display/configuration/', runScripts: 'outside-only'});
const window = dom.window; const saved = setupDefaults(); let requested = 0;
let buildProgress = {state: 'running', kit_id: 'b'.repeat(64)};
saved.capture.device = {type: 'Kraken', channel_count: 5, reference_channel: 0, surveillance_channels: [0, 1, 2, 3, 4], heimdall: {host: '127.0.0.1', port: 8091, control_port: 8092}};
saved.capture.fs = 2400000;
window.liveApiUrl = value => value; window.rememberApiPort = () => {}; window.fetchStatusResource = (url, options) => window.fetch(url, options);
window.fetch = async (url, options = {}) => {
  let body;
  if (url === '/api/config') body = saved;
  else if (url === '/api/config/capabilities') body = {editable: true, deviceProfiles: getDeviceProfiles(), fieldRules: FIELD_RULES};
  else if (url === '/api/config/validate') body = validateConfig(JSON.parse(options.body), saved);
  else if (url === '/api/system/status') body = {radar: 'no-data', errors: []};
  else if (url === '/api/upstream/status') body = {supported: false};
  else if (url === '/api/receivers/discover') body = {configRevision: 'a'.repeat(64), buildCapabilitiesKnown: true, managementAvailable: true, receivers: [{type: 'RspDuo', label: 'SDRplay RSPduo', capabilities: {liveCompiled: false, localBuildable: true, runtimeLoadable: false}, detection: {state: 'unknown'}, dependencies: {state: 'installed'}, managedService: {required: true, state: 'stopped'}, upstream: {availability: 'not-applicable'}, settings: [], setupGuide: []}], management: {actions: []}, errors: []};
  else if (url === '/api/sdrplay-build' && (!options.method || options.method === 'GET')) body = {buildable: true, ok: true, state: 'missing', kit_id: 'a'.repeat(64), reason: 'Local adapter has not been built.', progress: buildProgress};
  else if (url === '/api/sdrplay-build' && options.method === 'POST') { requested++; assert.equal(options.headers['X-VectorWarp-Intent'], 'sdrplay-local-build-v1'); assert.equal(options.body, '{}'); body = {ok: true, state: 'queued', message: 'Local RSPduo adapter build requested. Radar processing was not started.'}; }
  else throw new Error(`Unexpected ${url}`);
  return {ok: true, status: 200, headers: {get: () => '"' + 'a'.repeat(64) + '"'}, json: async () => JSON.parse(JSON.stringify(body))};
};
for (const file of ['kraken_geometry.js', 'config_ui.js']) window.eval(fs.readFileSync(path.join(__dirname, '../../html/js', file), 'utf8'));
const flush = () => new Promise(resolve => setImmediate(resolve));
(async () => {
  try {
    await window.renderConfiguration();
    const check = [...window.document.querySelectorAll('#receiver-setup button')].find(item => item.textContent === 'Check receiver software');
    check.click(); await flush(); await flush();
    const output = window.document.querySelector('#receiver-setup');
    assert.match(output.textContent, /Local adapter has not been built/);
    assert.equal(output.textContent.includes('running'), false, 'Progress for another kit must not override current helper status.');
    assert.ok([...output.querySelectorAll('a')].some(item => item.href === 'https://sdrplay.com/hardware-api/'));
    const build = [...output.querySelectorAll('button')].find(item => item.textContent === 'Build SDRplay support');
    assert.ok(build); build.click(); await flush(); await flush();
    assert.equal(requested, 1); assert.match(output.textContent, /Radar processing was not started/);
    const choose = [...output.querySelectorAll('button')].find(item => item.textContent === 'Choose SDRplay RSPduo for settings');
    assert.ok(choose, 'A local kit permits selection but does not claim compiled capture support.');
    buildProgress = {state:'failed', kit_id:'a'.repeat(64), reason:'Compiler version does not match the installed source kit.'};
    check.click(); await flush(); await flush();
    assert.match(output.textContent, /Compiler version does not match/, 'The user must see the actual build error.');
    console.log('SDRplay local build DOM fixture passed.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
