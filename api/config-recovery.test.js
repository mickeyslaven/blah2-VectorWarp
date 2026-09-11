'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const net = require('net');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {readConfig, saveConfig, writable} = require('./config-store');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'blah2-recovery-'));
const file = path.join(dir, 'config.yml');

function request(port, url, timeout = 500) {
  return new Promise((resolve, reject) => {
    const request = http.get(`http://127.0.0.1:${port}${url}`, {timeout}, response => {
      let data = '';
      response.on('data', chunk => { data += chunk; });
      response.on('end', () => resolve({status: response.statusCode, body: JSON.parse(data)}));
    });
    request.on('timeout', () => request.destroy(new Error(`request timed out after ${timeout}ms`)));
    request.on('error', reject);
  });
}

function availablePort(usedPorts) {
  const probe = net.createServer();
  return new Promise((resolve, reject) => {
    probe.once('error', reject);
    probe.listen(0, '127.0.0.1', () => {
      const {port} = probe.address();
      probe.close(error => {
        if (error) return reject(error);
        if (usedPorts.has(port)) return resolve(availablePort(usedPorts));
        usedPorts.add(port);
        resolve(port);
      });
    });
  });
}

(async () => {
  let child;
  const conflict = net.createServer();
  try {
    for (const name of ['config.yml', 'config-hackrf.yml', 'config-usrp.yml',
      'config-kraken.yml']) {
      const shipped = readConfig(path.join(__dirname, '..', 'config', name));
      assert.deepEqual(shipped.suppliedDefaults, [],
        `${name} omits required defaults: ${shipped.suppliedDefaults.join(', ')}`);
      if (name !== 'config-hackrf.yml') assert.equal(shipped.setupRequired, false,
        `${name} should be a complete first-run example`);
    }
    const missing = readConfig(file);
    assert.equal(missing.setupRequired, true);
    assert.equal(writable(file), true);
    assert.equal(fs.existsSync(file), false, 'Viewing first-run defaults must not create a file');
    for (const raw of ['capture: [broken', 'capture: null\nnetwork: null', 'x: &cycle {x: *cycle}', '42']) {
      fs.writeFileSync(file, raw);
      const recovery = readConfig(file);
      assert.equal(recovery.setupRequired, true);
      assert.ok(recovery.config.capture.device.type);
      assert.equal(fs.readFileSync(file, 'utf8'), raw, 'Recovery must not silently rewrite YAML');
    }
    fs.writeFileSync(file, 'capture: [broken', {mode: 0o640});
    fs.chmodSync(file, 0o640);
    const broken = readConfig(file);
    const repaired = saveConfig(file, broken.config, broken.revision);
    assert.equal(fs.readFileSync(`${file}.bak`, 'utf8'), 'capture: [broken');
    assert.equal(fs.statSync(file).mode & 0o777, 0o640);
    assert.equal(repaired.validation.valid, true);
    fs.chmodSync(file, 0o444);
    assert.equal(writable(file), false, 'Root must respect explicitly read-only settings');
    fs.chmodSync(file, 0o640);
    fs.chmodSync(dir, 0o555);
    assert.equal(writable(file), false, 'Atomic saves require an explicitly writable directory');
    fs.chmodSync(dir, 0o700);
    assert.throws(() => saveConfig(file, broken.config, broken.revision), /changed/);
    const optionalKraken = JSON.parse(JSON.stringify(repaired.config));
    optionalKraken.capture.fs = 2400000;
    optionalKraken.capture.device = {type: 'Kraken', channel_count: 8, reference_channel: 2};
    delete optionalKraken.process.performance;
    fs.writeFileSync(file, yaml.dump(optionalKraken));
    const inferred = readConfig(file).config;
    assert.equal(inferred.process.reference_synthesis.mode, 'dedicated');
    assert.deepEqual(inferred.capture.device.surveillance_channels, [0,1,3,4,5,6,7]);
    assert.deepEqual(inferred.process.reference_synthesis.channels, [2]);
    assert.equal(inferred.process.performance.surveillance_workers, 1);
    assert.equal(inferred.process.performance.fft_threads, 1);
    const legacyRsp = JSON.parse(JSON.stringify(repaired.config));
    delete legacyRsp.process.performance;
    fs.writeFileSync(file, yaml.dump(legacyRsp));
    const legacyDocument = readConfig(file);
    assert.deepEqual(legacyDocument.config.process.performance, {surveillance_workers: 1, fft_threads: 4, acceleration: 'auto'});
    assert.equal(legacyDocument.setupRequired, false, 'Valid old configs must not require setup just for optional defaults');

    await new Promise(resolve => conflict.listen(0, '0.0.0.0', resolve));
    const usedPorts = new Set([conflict.address().port]);
    const config = repaired.config;
    const apiPort = await availablePort(usedPorts);
    config.network.ports.api = apiPort;
    config.network.ports.map = conflict.address().port;
    for (const key of ['detection','track','timestamp','timing','iqdata','config'])
      config.network.ports[key] = await availablePort(usedPorts);
    fs.writeFileSync(file, yaml.dump(config));
    let childExit = null;
    let childOutput = '';
    const appendChildOutput = (label, chunk) => {
      childOutput = `${childOutput}${label}${chunk}`.slice(-4096);
    };
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), file],
      {stdio: ['ignore', 'pipe', 'pipe']});
    child.stdout.on('data', chunk => appendChildOutput('stdout: ', chunk));
    child.stderr.on('data', chunk => appendChildOutput('stderr: ', chunk));
    child.once('error', error => { childExit = `spawn error: ${error.message}`; });
    child.once('exit', (code, signal) => { childExit = `exit ${code}${signal ? ` (${signal})` : ''}`; });
    let response;
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline && childExit === null) {
      try { response = await request(apiPort, '/api/system/status'); break; }
      catch (_) { await new Promise(resolve => setTimeout(resolve, 30)); }
    }
    assert.equal(response?.status, 200, `API stays available when a radar data port cannot bind (${childExit || 'startup timed out'}; ${childOutput || 'no child output'})`);
    assert.ok(response.body.errors.some(error => error.includes('map data port')));
    assert.equal((await request(apiPort, '/api/config/capabilities')).body.editable, true);
    fs.writeFileSync(file, 'not: [valid yaml');
    assert.equal((await request(apiPort, '/api/config/capabilities')).body.setupRequired, true);
    console.log('First-run, malformed-config recovery, atomic backup, permissions, stale revision and occupied-port tests passed.');
  } finally {
    if (child) child.kill('SIGTERM');
    conflict.close();
    fs.rmSync(dir, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
