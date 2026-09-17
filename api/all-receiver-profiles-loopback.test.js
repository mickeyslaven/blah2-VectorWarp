'use strict';
// Real API/config transactions across every profile.  The only upstream is a
// loopback Kraken control fixture; no SDR, vendor runtime, or service is used.
const assert = require('assert/strict');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');

const root = path.join(__dirname, '..');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-all-receivers-'));
const filename = path.join(directory, 'config.yml');
const marker = path.join(directory, 'restart-marker');
let apiPort, suitePort;
const clone = value => JSON.parse(JSON.stringify(value));
const load = name => yaml.load(fs.readFileSync(path.join(root, 'config', name), 'utf8'));
const initial = load('config-kraken.yml');
initial.network.ip = '127.0.0.1';

const suiteState = {settings: {center_freq: initial.capture.fc, sample_rate: initial.capture.fs, gain: 0},
  num_channels: initial.capture.device.channel_count, max_elements: 8,
  operating_mode: 'coherent', reconfiguring: false, recovering: false};
const suite = net.createServer(socket => {
  socket.write(`${JSON.stringify(suiteState)}\n`);
  socket.setEncoding('utf8'); let pending = '';
  socket.on('data', block => {
    pending += block; const lines = pending.split('\n'); pending = lines.pop();
    for (const line of lines.filter(Boolean)) {
      const command = JSON.parse(line);
      if (command.command === 'set_frequency') suiteState.settings.center_freq = command.frequency;
      else if (command.command === 'set_num_elements') suiteState.num_channels = command.num_elements;
      else if (command.command === 'set_gain') suiteState.settings.gain = command.gain;
      else throw new Error(`Unexpected simulated Suite command ${command.command}`);
      socket.write(`${JSON.stringify({status: 'success', ...command})}\n${JSON.stringify(suiteState)}\n`);
    }
  });
});

let child;
let stderr = '';
let revision;
async function reservePorts(count) {
  const listeners = await Promise.all(Array.from({length: count}, () => new Promise((resolve, reject) => {
    const listener = net.createServer();
    listener.once('error', reject);
    listener.listen(0, '127.0.0.1', () => resolve(listener));
  })));
  const ports = listeners.map(listener => listener.address().port);
  await Promise.all(listeners.map(listener => new Promise(resolve => listener.close(resolve))));
  return ports;
}
function request(method, pathname, body, headers = {}) {
  return new Promise((resolve, reject) => {
    const payload = body === undefined ? null : JSON.stringify(body);
    const req = http.request({hostname: '127.0.0.1', port: apiPort, path: pathname, method,
      headers: {...(payload ? {'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload)} : {}),
        ...(revision ? {'If-Match': `"${revision}"`} : {}),
        ...(['PUT', 'POST'].includes(method) ? {Origin: `http://127.0.0.1:${apiPort}`} : {}),
        ...(method === 'PUT' ? {'X-VectorWarp-Intent': 'config-write-v1'} : {}),
        ...headers}}, response => {
      let text = ''; response.setEncoding('utf8'); response.on('data', block => { text += block; });
      response.on('end', () => resolve({status: response.statusCode, body: text ? JSON.parse(text) : null}));
    });
    req.on('error', reject); if (payload) req.write(payload); req.end();
  });
}
async function waitForApi() {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    try { const response = await request('GET', '/api/config/capabilities'); if (response.status === 200) return response; }
    catch (_) {}
    await new Promise(resolve => setTimeout(resolve, 30));
  }
  throw new Error(`loopback API did not start: ${stderr}`);
}
function profile(name, type) {
  const config = load(name);
  config.network = clone(initial.network);
  if (type === 'Kraken') {
    config.capture.device.heimdall.host = '127.0.0.1';
    config.capture.device.heimdall.control_port = suitePort;
  }
  if (type === 'HackRF') config.capture.device.serial = ['0001', '0002'];
  return config;
}

(async () => {
  try {
    const ports = await reservePorts(Object.keys(initial.network.ports).length + 1);
    Object.keys(initial.network.ports).forEach((key, index) => { initial.network.ports[key] = ports[index]; });
    apiPort = initial.network.ports.api;
    suitePort = ports.at(-1);
    initial.capture.device.heimdall.host = '127.0.0.1';
    initial.capture.device.heimdall.control_port = suitePort;
    initial.capture.device.heimdall.gain = 49.6;
    fs.writeFileSync(filename, yaml.dump(initial));
    await new Promise((resolve, reject) => { suite.once('error', reject); suite.listen(suitePort, '127.0.0.1', resolve); });
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), filename], {stdio: ['ignore', 'ignore', 'pipe'],
      env: {...process.env, BLAH2_RECEIVER_TYPES: 'Kraken,RspDuo,Usrp,HackRF',
        BLAH2_CONFIG_RESTART_COMMAND: JSON.stringify([process.execPath, '-e',
          `require('fs').writeFileSync(${JSON.stringify(marker)}, 'unexpected restart')`])}});
    child.stderr.on('data', value => { stderr += value; });
    const capabilities = await waitForApi(); revision = capabilities.body.configRevision;
    assert.deepEqual(capabilities.body.compiledLiveTypes, ['Kraken', 'RspDuo', 'Usrp', 'HackRF']);
    assert.equal(capabilities.body.setupRequired, false,
      'a valid numeric Kraken gain must not mark Settings as incomplete');
    let readback = await request('GET', '/api/config');
    assert.equal(readback.status, 200);
    assert.equal(readback.body.capture.device.heimdall.gain, 49.6,
      'the Settings API must return the numeric gain present at API startup');
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.heimdall.gain, 49.6);

    for (const [name, type] of [['config-kraken.yml', 'Kraken'], ['config.yml', 'RspDuo'],
      ['config-usrp.yml', 'Usrp'], ['config-hackrf.yml', 'HackRF']]) {
      const candidate = profile(name, type);
      const validation = await request('POST', '/api/config/validate', candidate);
      assert.equal(validation.status, 200, `${type}: ${JSON.stringify(validation.body)}`);
      const headers = type === 'Kraken' ? {'X-VectorWarp-Receiver-Sync': 'synchronize-v1'} : {};
      const saved = await request('PUT', '/api/config?restart=false', candidate, headers);
      assert.equal(saved.status, 200, `${type}: ${JSON.stringify(saved.body)}`);
      assert.equal(saved.body.config.capture.device.type, type);
      assert.equal(saved.body.restarting, false);
      revision = saved.body.revision;
      assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.type, type);
    }
    for (const gain of [28.7, -1, 'keep']) {
      const candidate = profile('config-kraken.yml', 'Kraken');
      candidate.capture.device.heimdall.gain = gain;
      const saved = await request('PUT', '/api/config?restart=false', candidate,
        {'X-VectorWarp-Receiver-Sync': 'synchronize-v1'});
      assert.equal(saved.status, 200, `Kraken gain ${gain}: ${JSON.stringify(saved.body)}`);
      assert.equal(saved.body.config.capture.device.heimdall.gain, gain,
        'save response must match the requested gain');
      revision = saved.body.revision;
      readback = await request('GET', '/api/config');
      assert.equal(readback.body.capture.device.heimdall.gain, gain,
        'Settings API readback must match the saved gain');
      assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.heimdall.gain, gain,
        'Settings API must agree with raw YAML');
      assert.equal((await request('GET', '/api/config/capabilities')).body.setupRequired, false);
    }
    child.kill('SIGTERM');
    await new Promise(resolve => child.once('exit', resolve));
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), filename], {stdio: ['ignore', 'ignore', 'pipe'],
      env: {...process.env, BLAH2_RECEIVER_TYPES: 'Kraken,RspDuo,Usrp,HackRF',
        BLAH2_CONFIG_RESTART_COMMAND: JSON.stringify([process.execPath, '-e',
          `require('fs').writeFileSync(${JSON.stringify(marker)}, 'unexpected restart')`])}});
    child.stderr.on('data', value => { stderr += value; });
    const restarted = await waitForApi(); revision = restarted.body.configRevision;
    assert.equal(restarted.body.setupRequired, false,
      'cold API startup must accept the saved gain without requiring setup');
    readback = await request('GET', '/api/config');
    assert.equal(readback.body.capture.device.heimdall.gain, 'keep',
      'cold API startup must return the raw saved gain');
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.heimdall.gain, 'keep');
    assert.equal(fs.existsSync(marker), false, 'restart=false must never invoke the restart command');
    console.log('All four receiver profiles validated and saved through loopback API; only Kraken used a simulated upstream.');
  } finally {
    if (child) { child.kill('SIGTERM'); if (child.exitCode === null) await new Promise(resolve => child.once('exit', resolve)); }
    await new Promise(resolve => suite.close(resolve));
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
