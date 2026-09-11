'use strict';
// Actual non-preview API and YAML store, browser DOM, silent loopback receiver.
// No real SDR, helper, package manager or service manager is accessed.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const net = require('net');
const {spawn} = require('child_process');
const {once} = require('events');
const {JSDOM} = require('jsdom');
const {readConfig} = require('../../api/config-store');
const {writeConfigAtomically} = require('../../api/config-manager');
const yaml = require('js-yaml');
const root = path.resolve(__dirname, '../..');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-pending-'));
const filename = path.join(directory, 'config.yml');
const restartMarker = path.join(directory, 'restart-requested');
const fsyncMarker = path.join(directory, 'fail-directory-fsync');
const preload = path.join(directory, 'persistence-injection.js');
fs.writeFileSync(preload, `const fs = require('fs'); const original = fs.fsyncSync; const rename = fs.renameSync; let renamedConfig = false;
fs.renameSync = function(from, to) { const result = rename.call(fs, from, to); renamedConfig = to === ${JSON.stringify(filename)}; return result; };
fs.fsyncSync = function(fd) { if (renamedConfig && fs.fstatSync(fd).isDirectory() && fs.existsSync(${JSON.stringify(fsyncMarker)}))
  throw new Error('simulated directory fsync failure after rename'); return original.call(fs, fd); };`);
const config = yaml.load(fs.readFileSync(path.join(root, 'config/config-kraken.yml'), 'utf8'));
let child, dom, base, stderr = '', connections = 0, bytes = 0;
const sockets = new Set(), reservations = [];
const receiver = net.createServer(socket => {
  connections++; sockets.add(socket);
  socket.on('data', value => { bytes += value.length; });
  socket.on('close', () => sockets.delete(socket));
});
const request = (url, options = {}) => new Promise((resolve, reject) => {
  const req = http.request(`${base}${url}`, {method: options.method || 'GET',
    headers: {Origin: base, ...options.headers}}, response => {
    let text = ''; response.setEncoding('utf8'); response.on('data', value => { text += value; });
    response.on('end', () => resolve({ok: response.statusCode >= 200 && response.statusCode < 300,
      status: response.statusCode, headers: {get: name => response.headers[name.toLowerCase()] || null},
      json: async () => JSON.parse(text)}));
  });
  req.on('error', reject); if (options.body) req.write(options.body); req.end();
});
async function start() {
  child = spawn(process.execPath, ['--require', preload, path.join(root, 'api/server.js'), filename], {
    env: {...process.env, BLAH2_PREVIEW: 'false', BLAH2_SETUP_PORT: String(config.network.ports.api),
      BLAH2_RECEIVER_TYPES: 'Kraken', BLAH2_RECEIVER_STATUS_TIMEOUT_MS: '100',
      BLAH2_CONFIG_RESTART_COMMAND: JSON.stringify([process.execPath, '-e',
        `require('fs').writeFileSync(${JSON.stringify(restartMarker)}, 'unexpected restart')`])},
    stdio: ['ignore', 'ignore', 'pipe']});
  child.stderr.on('data', value => { stderr += value; });
  for (let attempt = 0; attempt < 100; attempt++) {
    try { if ((await request('/api/config/capabilities')).ok) return; } catch (_) { /* Starting isolated API. */ }
    await new Promise(resolve => setTimeout(resolve, 25));
  }
  throw new Error(`API failed to start: ${stderr}`);
}
async function stop() { if (child) { const stopped = once(child, 'exit'); child.kill('SIGTERM'); await stopped; child = null; } }
async function put(mode, value, revision = readConfig(filename).revision, extraHeaders = {}, restart = 'false') {
  return request(`/api/config?${mode ? `mode=${mode}&` : ''}restart=${restart}`, {
    method: 'PUT', headers: {'Content-Type': 'application/json', 'If-Match': `"${revision}"`,
      'X-VectorWarp-Receiver-Sync': mode === 'pending' ? 'save-pending-v1' : 'synchronize-v1', ...extraHeaders},
    body: JSON.stringify(value)});
}
async function browser() {
  if (dom) dom.window.close();
  dom = new JSDOM('<div id="configuration"></div>', {url: `${base}/display/configuration/`, runScripts: 'outside-only'});
  const window = dom.window;
  window.liveApiUrl = value => value; window.rememberApiPort = () => {};
  window.fetch = async (url, options = {}) => {
    // The separate periodic status display is read-only and is not this save
    // transaction. Keep it offline so receiver connection counts are exact.
    if (url === '/api/upstream/status') return {ok: false, json: async () => ({supported: true, available: false})};
    assert.ok(!url.startsWith('/api/receivers'), 'Save for later must never call management');
    return request(url, options);
  };
  window.fetchStatusResource = (url, options) => window.fetch(url, options);
  for (const file of ['html/js/kraken_geometry.js', 'html/js/config_ui.js']) window.eval(fs.readFileSync(path.join(root, file), 'utf8'));
  await window.renderConfiguration();
  return window;
}
(async () => {
  try {
    config.network.ip = '127.0.0.1'; config.truth.adsb.enabled = false;
    for (const name of Object.keys(config.network.ports)) {
      const reservation = net.createServer(); reservations.push(reservation);
      await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
      config.network.ports[name] = reservation.address().port;
    }
    await new Promise(resolve => receiver.listen(0, '127.0.0.1', resolve));
    // This reserved, unbound field is read-only even during first-run setup.
    config.network.ports.config = 4003;
    config.capture.device.heimdall.host = '127.0.0.1';
    config.capture.device.heimdall.control_port = receiver.address().port;
    config.capture.replay.state = false;
    base = `http://127.0.0.1:${config.network.ports.api}`;
    await Promise.all(reservations.map(server => new Promise(resolve => server.close(resolve))));
    writeConfigAtomically(filename, config);
    await start();
    // Exercise genuinely missing first-run YAML as well as offline live Kraken.
    fs.unlinkSync(filename);
    assert.equal((await (await request('/api/config/capabilities')).json()).setupRequired, true);
    const firstRun = await put('pending', config);
    assert.equal(firstRun.status, 200, JSON.stringify(await firstRun.json()));
    assert.equal(connections, 0); assert.equal(bytes, 0);
    let window = await browser();
    const query = key => window.document.querySelector(`[data-path="${key}"] input, [data-path="${key}"] select`);
    const change = (key, value) => { const input = query(key); assert.ok(input, key); input.value = String(value); input.dispatchEvent(new window.Event('input', {bubbles: true})); };
    change('capture.device.heimdall.host', '127.0.0.2');
    change('process.data.cpi', .4);
    assert.equal(await window.validateActiveConfiguration(), true);
    assert.equal(window.document.getElementById('config-save-later').textContent, 'Save for later');
    await window.saveConfiguration('pending');
    assert.match(window.document.getElementById('config-message').textContent, /No receiver commands or restart/);
    assert.equal(readConfig(filename).config.capture.device.heimdall.host, '127.0.0.2');
    assert.equal(readConfig(filename).config.capture.replay.state, false);
    assert.equal(connections, 0); assert.equal(bytes, 0); assert.equal(fs.existsSync(restartMarker), false);
    assert.equal(window.document.getElementById('config-save').disabled, false, 'Unchanged pending settings must still offer Apply');
    window = await browser();
    assert.equal(query('capture.device.heimdall.host').value, '127.0.0.2');
    assert.match(window.document.getElementById('config-state').textContent, /pending/);
    const beforeApply = fs.readFileSync(filename, 'utf8');
    await window.saveConfiguration();
    assert.match(window.document.getElementById('config-message').textContent, /connection failed/i);
    assert.equal(fs.readFileSync(filename, 'utf8'), beforeApply, 'Offline Apply must not fall back to pending save');
    assert.equal(fs.existsSync(restartMarker), false);
    window.close(); dom = null;
    let candidate = readConfig(filename).config;
    candidate.capture.device.heimdall.host = '127.0.0.1';
    assert.equal((await put('pending', candidate)).status, 200);
    const revision = readConfig(filename).revision;
    for (const options of [
      {headers: {'X-VectorWarp-Receiver-Sync': 'synchronize-v1'}},
      {headers: {Origin: 'http://untrusted.example'}, expected: 403}, {restart: 'true'}
    ]) assert.equal((await put('pending', candidate, revision, options.headers, options.restart)).status, options.expected || 428);
    assert.equal((await put('pending', candidate, 'stale')).status, 409);
    assert.equal((await put('unknown', candidate)).status, 422);
    assert.equal(connections, 0);
    const beforeFailure = fs.readFileSync(filename, 'utf8');
    fs.unlinkSync(`${filename}.bak`); fs.mkdirSync(`${filename}.bak`);
    candidate.process.data.cpi = .3;
    const failure = await put('pending', candidate);
    assert.equal(failure.status, 500);
    assert.equal(fs.readFileSync(filename, 'utf8'), beforeFailure);
    assert.equal((await failure.json()).receiverSync.configPersisted, null, 'Failed persistence must not return a successful pending receipt');
    assert.equal(connections, 0); assert.equal(bytes, 0);
    fs.rmdirSync(`${filename}.bak`);
    assert.equal((await put('pending', candidate)).status, 409, 'Unknown persistence cannot be erased by another pending save');
    // A separate isolated API/journal starts the independent post-rename case.
    // This is fixture reset, never a recovery option in the application.
    await stop(); fs.unlinkSync(`${filename}.receiver-state.json`); await start();
    assert.equal((await put('pending', candidate)).status, 200);
    fs.writeFileSync(fsyncMarker, 'inject');
    candidate.process.data.cpi = .2;
    const renamedFailure = await put('pending', candidate);
    assert.equal(renamedFailure.status, 500);
    assert.equal((await renamedFailure.json()).receiverSync.status, 'pending-save-unknown');
    assert.equal(readConfig(filename).config.process.data.cpi, .2, 'Post-rename failure may already have replaced YAML');
    assert.equal(connections, 0); assert.equal(bytes, 0); assert.equal(fs.existsSync(restartMarker), false);
    window = await browser();
    change('process.data.cpi', .15);
    await window.saveConfiguration('pending');
    assert.match(window.document.getElementById('config-message').textContent, /unresolved outcome|Reconcile/i);
    assert.equal(readConfig(filename).config.process.data.cpi, .2, 'Unresolved receipt prevents another pending mutation');
    window.close(); dom = null;
    fs.unlinkSync(fsyncMarker);
    await stop(); await start();
    const restarted = await (await request('/api/system/status')).json();
    assert.equal(restarted.receiverSynchronization.state, 'unknown');
    assert.equal(restarted.receiverSynchronization.applicationVerified, false);
    assert.equal(restarted.receiverSynchronization.reconciliationRequired, true);
    assert.equal(restarted.receiverSynchronization.receipt.status, 'pending-save-unknown');
    window = await browser();
    assert.equal(await window.validateActiveConfiguration(), true);
    assert.equal(window.document.getElementById('config-save').disabled, false);
    await window.saveConfiguration();
    assert.equal(connections, 1, 'Unchanged Apply after restart must open a fresh receiver observation');
    assert.equal(bytes, 0, 'Silent receiver never supplies an initial status authorizing a command');
    assert.equal(fs.existsSync(restartMarker), false);
    assert.equal((await (await request('/api/system/status')).json()).restart.state, 'idle');
    console.log('Non-preview first-run/pending API+DOM: YAML reload, no receiver/helper/restart, explicit intent, persistence failure, and fresh Apply after API restart passed.');
  } finally {
    if (dom) dom.window.close(); await stop();
    for (const socket of sockets) socket.destroy();
    await new Promise(resolve => receiver.close(resolve));
    for (const server of reservations) if (server.listening) server.close();
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => { console.error(error.stack || error); if (stderr) console.error(stderr); process.exitCode = 1; });
