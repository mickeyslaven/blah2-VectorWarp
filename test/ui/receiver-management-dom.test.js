'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {JSDOM} = require('jsdom');
const {setupDefaults} = require('../../api/config-store');
const {getDeviceProfiles, FIELD_RULES, validateConfig} = require('../../api/config-manager');
const dom = new JSDOM('<div id="configuration"></div>', {url: 'http://127.0.0.1:3000/display/configuration/', runScripts: 'outside-only'});
const window = dom.window, saved = setupDefaults(), requests = [];
saved.capture.device = {type: 'Kraken', channel_count: 5, reference_channel: 0,
  surveillance_channels: [0, 1, 2, 3, 4], heimdall: {host: '127.0.0.1', port: 8091, control_port: 8092}};
saved.capture.fs = 2400000;
let revision = 'a'.repeat(64), discoveryCount = 0, mode = 'ok', directMacAction = false;
window.liveApiUrl = value => value;
window.rememberApiPort = () => {};
window.fetchStatusResource = async (url, options) => window.fetch(url, options);
window.fetch = async (url, options = {}) => {
  requests.push({url, options});
  let body, status = 200;
  if (url === '/api/config') body = saved;
  else if (url === '/api/config/capabilities') body = {editable: true, deviceProfiles: getDeviceProfiles(), fieldRules: FIELD_RULES};
  else if (url === '/api/config/validate') body = validateConfig(JSON.parse(options.body), saved);
  else if (url === '/api/system/status') body = {radar: 'no-data', errors: []};
  else if (url === '/api/upstream/status') body = {supported: false};
  else if (url === '/api/receivers/discover') {
    discoveryCount++;
    body = {configRevision: revision, buildCapabilitiesKnown: true, managementAvailable: true,
      receivers: ['Kraken', 'RspDuo', 'Usrp', 'HackRF'].map(type => ({type, label: type,
        capabilities: {liveCompiled: ['Kraken', 'HackRF'].includes(type),
          runtimeLoadable: type === 'RspDuo' ? false : true}, detection: {state: 'unknown'},
        dependencies: {state: 'unknown'}, managedService: {required: false},
        upstream: {availability: type === 'Kraken' ? 'available' : 'not-applicable'},
        settings: type === 'Kraken' ? [
          {configField: 'capture.fc', direction: 'browser-to-upstream-after-ack-and-readback'},
          {configField: 'capture.fs', direction: 'upstream-authoritative-mismatch-block'},
          {configField: 'capture.device.reference_channel', direction: 'config-only'}] :
          [{configField: 'capture.fc', direction: 'direct-tuning'},
            ...(['RspDuo', 'HackRF'].includes(type) ? [{configField: 'capture.device.serial', direction: 'direct-tuning'}] : [])]})),
      management: {actions: [{id: 'reviewed-hackrf', receiverType: 'HackRF', kind: 'install-packages', available: true}]}, errors: []};
  } else if (url === '/api/receivers/plan') body = directMacAction ? {nonce: 'c'.repeat(64), configRevision: revision,
    status: 'ready', lifetimeSeconds: 300, review: 'Homebrew will install the open-source HackRF formula.', requiresAuthorization: false} :
    {nonce: 'b'.repeat(64), configRevision: revision, status: 'awaiting-local-authorization', lifetimeSeconds: 300, review: '<img src=x onerror=alert(1)>',
      authorizationCommand: 'sudo /opt/vectorwarp/libexec/vectorwarp-receiver-helper authorize fixture',
      transaction: {changes: [{name: 'hackrf', version: '1.2.3', origin: 'Fedora', archive: 'updates'}]}};
  else if (url === '/api/receivers/execute') {
    if (mode === 'transport') throw new Error('Connection lost; action outcome unknown.');
    body = {ok: true, status: 'complete', message: 'Verified software state.'};
  } else throw new Error(`Unexpected ${url}`);
  return {ok: status === 200, status, headers: {get: () => `"${revision}"`}, json: async () => JSON.parse(JSON.stringify(body))};
};
for (const file of ['kraken_geometry.js', 'config_ui.js']) window.eval(fs.readFileSync(path.join(__dirname, '../../html/js', file), 'utf8'));
const flush = () => new Promise(resolve => setImmediate(resolve));
const findButton = label => [...window.document.querySelectorAll('#receiver-setup button')].find(item => item.textContent === label);
async function click(label) { const button = findButton(label); assert.ok(button, label); button.click(); await flush(); await flush(); }
(async () => {
  try {
    await window.renderConfiguration();
    const suiteGain = window.document.querySelector('[data-path="capture.device.heimdall.gain"]');
    assert.ok(suiteGain, 'Older saved Kraken files still show the non-retuning gain control');
    const gainControls = suiteGain.querySelectorAll('select,input');
    assert.equal(gainControls[0].value, 'keep');
    assert.equal(gainControls[1].hidden, true);
    assert.equal(discoveryCount, 0, 'Host checks only run on an explicit check');
    await click('Check receiver software');
    const text = window.document.querySelector('#receiver-setup').textContent;
    for (const type of ['Kraken', 'RspDuo', 'Usrp', 'HackRF']) assert.ok(text.includes(type));
    assert.ok([...window.document.querySelectorAll('#receiver-setup a')].some(link =>
      link.href === 'https://sdrplay.com/hardware-api/'), 'Unverified SDK state exposes the official vendor link.');
    assert.ok(text.includes('will be reused'));
    assert.ok(text.includes('software cannot load'));
    for (const type of ['Kraken', 'RspDuo', 'Usrp', 'HackRF'])
      await click(`How settings apply: ${type}`);
    const matrix = window.document.querySelector('#receiver-setup').textContent;
    assert.match(matrix, /sent to Suite V2, then checked against its reply and updated status/);
    assert.match(matrix, /not sent to Suite V2/);
    assert.match(matrix, /SDRplay API v3 at startup/);
    assert.match(matrix, /UHD after Save & Restart/);
    assert.match(matrix, /both HackRFs at startup/);
    assert.match(matrix, /tuner values are not read back/);
    assert.match(matrix, /RSPduo serial: applied through SDRplay/);
    assert.match(matrix, /HackRF serial numbers: applied to both HackRFs/);
    assert.doesNotMatch(matrix, /RSPduo serial: applied to both HackRFs/);
    assert.doesNotMatch(matrix, /capture\.fc/, 'Setting help uses the same label as the form');
    await click('Review missing dependency install');
    assert.ok(window.document.querySelector('#receiver-setup pre').textContent.startsWith('sudo '));
    assert.match(window.document.querySelector('#receiver-setup').textContent, /hackrf.*1\.2\.3.*Fedora/s,
      'The full native package transaction must be shown before execution');
    assert.equal(window.document.querySelectorAll('#receiver-setup img').length, 0, 'Receipt text cannot inject markup');
    await click('Run authorized action');
    assert.equal(requests.filter(item => item.url === '/api/receivers/execute').length, 1);
    const sent = requests.find(item => item.url === '/api/receivers/execute').options;
    assert.equal(sent.headers['X-VectorWarp-Intent'], 'receiver-management-v1');
    assert.deepEqual(JSON.parse(sent.body), {nonce: 'b'.repeat(64), configRevision: revision});
    assert.equal(requests.filter(item => item.options.method === 'PUT').length, 0, 'Software checks never rewrite SDR endpoints or settings');
    assert.equal(discoveryCount, 2, 'Successful management always triggers fresh discovery');
    await click('Review missing dependency install'); mode = 'transport';
    await click('Run authorized action');
    assert.ok(window.document.querySelector('#receiver-setup').textContent.includes('outcome unknown'));
    assert.equal(findButton('Run authorized action'), undefined, 'Lost transport cannot silently retry a consumed action');
    mode = 'ok'; directMacAction = true;
    await click('Check receiver software');
    await click('Review missing dependency install');
    assert.equal(window.document.querySelector('#receiver-setup pre'), null, 'macOS reviewed Homebrew actions do not show a Linux sudo helper command');
    await click('Run reviewed action');
    const macSent = requests.filter(item => item.url === '/api/receivers/execute').at(-1);
    assert.deepEqual(JSON.parse(macSent.options.body), {nonce: 'c'.repeat(64), configRevision: revision});
    console.log('Receiver software browser flow passed: four backends, reuse, explicit plan/grant/execute, escaped receipts and lost-transport handling.');
  } finally { window.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
