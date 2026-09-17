'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('jsdom');
const dom = new JSDOM('<div id="config-diagnostics"></div><div id="acceleration-status"></div>',
  {url: 'http://localhost:3000', runScripts: 'outside-only', pretendToBeVisual: true});
const window = dom.window;
let state = {radar: 'receiving', configRevision: 'one', loadedRevision: 'one', errors: [],
  acceleration: {active: 'vulkan', state: 'ready', device: 'V3D 4.2'},
  clutterAcceleration: {active: 'cpu', state: 'fallback', reason: 'Clutter mismatch'},
  gpuSetup: {pi: true, state: 'compiler-risk', message: 'Check the distro driver <img src=x onerror=alert(1)>',
    command: 'untrusted remote command', serviceAccess: {state: 'group-access-needed'}}};
window.liveApiUrl = value => value;
window.fetch = async () => ({ok: true, json: async () => state});
window.fetchStatusResource = (url, options) => window.fetch(url, options);
window.eval(fs.readFileSync(path.resolve(__dirname, '../../html/js/config_ui.js'), 'utf8'));
(async () => {
  await window.refreshConfigDiagnostics();
  const target = window.document.getElementById('acceleration-status');
  assert.match(target.textContent, /Delay–Doppler: GPU: V3D 4.2/);
  assert.match(target.textContent, /Clutter: CPU — Clutter mismatch/);
  assert.match(target.textContent, /--install-driver/); assert.match(target.textContent, /--enable-service-access/);
  assert(!target.querySelector('img')); assert(!target.textContent.includes('untrusted remote command'));
  assert.equal(target.querySelectorAll('button').length, 0, 'No privileged browser action');
  state = {...state, clutterAcceleration: {...state.acceleration},
    gpuSetup: {...state.gpuSetup, state: 'qualified', message: 'Current instance qualified', serviceAccess: {state: 'available'}}};
  await window.refreshConfigDiagnostics();
  assert.match(target.textContent, /Clutter: GPU/); assert(!target.textContent.includes('--install-driver'));
  state = {...state, gpuSetup: {pi: false, state: 'service-access-needed', message: 'Processor account needs GPU access',
    serviceAccess: {state: 'group-access-needed'}}};
  await window.refreshConfigDiagnostics();
  assert.match(target.textContent, /Processor account needs GPU access/);
  assert.match(target.textContent, /--enable-service-access/);
  assert(!target.textContent.includes('--install-driver'), 'Desktop GPUs must not get Pi driver commands');
  state = {...state, gpuSetup: {pi: false, state: 'driver-unverified', message: 'Activate the updated receiver helper',
    serviceAccess: {state: 'unavailable'}}};
  await window.refreshConfigDiagnostics();
  assert.match(target.textContent, /Activate the updated receiver helper/);
  assert(!target.textContent.includes('--install-driver'));
  assert(!target.textContent.includes('--enable-service-access'), 'Unknown access must not be reported as a missing group');
  state = {...state, radar: 'stale', gpuSetup: {pi: false}};
  await window.refreshConfigDiagnostics();
  assert.match(target.textContent, /waiting for radar/); assert(!target.textContent.includes('GPU: V3D'));
  dom.window.close();
  console.log('Pi GPU setup DOM: independent stages, safe instructions, no privilege, stale state PASS');
})().catch(error => { dom.window.close(); console.error(error); process.exitCode = 1; });
