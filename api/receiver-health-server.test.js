'use strict';

// Hardware-free loopback contract. Runs on Linux and macOS through the same
// production ingestion/status routes consumed by the common browser status.
const assert = require('assert/strict');
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const idle = {active: false, detectedCount: 0, correctedCount: 0,
  lastDetectedAt: 0, lastCorrectedAt: 0};
const health = {schema: 1, receiver: 'RspDuo', monitoring: true,
  overload: {A: {...idle}, B: {...idle}}, ackFailures: 0};
const live = {input: 'live', receiver: 'RspDuo', state: 'live', error: '', file: '',
  sampleRate: 2000000, frequency: 551000000, positionSamples: 0, totalSamples: 0,
  loops: 0, trailingSamples: 0, recording: false, recordingFile: '',
  recordingError: '', recordedSamples: 0, recordingRequestId: 0};

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

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-health-api-'));
  const filename = path.join(directory, 'config.yml');
  let child;
  let errors = '';
  try {
    const config = yaml.load(fs.readFileSync(path.join(__dirname, '../config/config-pi4-rspduo.yml'), 'utf8'));
    config.network.ip = '127.0.0.1';
    const ports = await reservePorts(Object.keys(config.network.ports).length);
    Object.keys(config.network.ports).forEach((name, index) => { config.network.ports[name] = ports[index]; });
    fs.writeFileSync(filename, yaml.dump(config));
    const original = fs.readFileSync(filename);
    const origin = `http://127.0.0.1:${config.network.ports.api}`;
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), filename], {
      env: {...process.env, BLAH2_PREVIEW: 'true', BLAH2_RECEIVER_TYPES: 'RspDuo',
        BLAH2_CONFIG_RESTART_COMMAND: ''}, stdio: ['ignore', 'ignore', 'pipe']});
    child.stderr.on('data', chunk => { errors += chunk; });
    const systemStatus = async () => {
      const response = await fetch(`${origin}/api/system/status`);
      assert.equal(response.status, 200);
      return response.json();
    };
    const post = (payload, headers = {}) => fetch(`${origin}/api/processor/status`, {
      method: 'POST', headers: {'Content-Type': 'application/json', ...headers},
      body: JSON.stringify(payload)});
    let ready = false;
    for (let attempt = 0; attempt < 100; attempt++) {
      try { await systemStatus(); ready = true; break; } catch (_) { await delay(30); }
    }
    assert.equal(ready, true, `API failed to start: ${errors}`);
    let current = await systemStatus();
    assert.equal(current.processorFresh, false);
    assert.equal(current.processor, null);

    assert.equal((await post({...live, receiverHealth: health})).status, 204);
    current = await systemStatus();
    assert.equal(current.processorFresh, true);
    assert.deepEqual(current.processor.receiverHealth, health);
    const firstReceivedAt = current.processor.receivedAt;

    const detectedAt = Date.now();
    const active = {...health, overload: {A: {...idle}, B: {
      ...idle, active: true, detectedCount: 1, lastDetectedAt: detectedAt}}, ackFailures: 1};
    await delay(10);
    assert.equal((await post({...live, receiverHealth: active})).status, 204);
    current = await systemStatus();
    assert.deepEqual(current.processor.receiverHealth, active);
    assert.ok(current.processor.receivedAt > firstReceivedAt, 'Each accepted update refreshes its receipt time');

    const corrected = {...active, overload: {...active.overload, B: {...active.overload.B,
      active: false, correctedCount: 1, lastCorrectedAt: Date.now()}}};
    assert.equal((await post({...live, receiverHealth: corrected})).status, 204);
    assert.deepEqual((await systemStatus()).processor.receiverHealth, corrected);
    const stopped = {...corrected, monitoring: false};
    assert.equal((await post({...live, state: 'stopped', receiverHealth: stopped})).status, 204);
    current = await systemStatus();
    assert.equal(current.processor.state, 'stopped');
    assert.deepEqual(current.processor.receiverHealth, stopped);

    assert.equal((await post(live)).status, 204);
    current = await systemStatus();
    assert.equal(current.processorFresh, true);
    assert.equal(Object.hasOwn(current.processor, 'receiverHealth'), false,
      'A valid old-module update removes the prior health snapshot');
    assert.equal((await post({...live, receiverHealth: health})).status, 204);
    const beforeRejectedOrigin = (await systemStatus()).processor;
    assert.equal((await post({...live, receiverHealth: null}, {Origin: 'https://unrelated.example'})).status, 403);
    assert.deepEqual((await systemStatus()).processor, beforeRejectedOrigin,
      'Untrusted origins cannot invalidate accepted receiver telemetry');

    for (const receiverHealth of [null, {...health, overload: {A: idle}},
      {...health, overload: {A: {...idle, active: true}, B: idle}}]) {
      const rejected = await post({...live, receiverHealth});
      assert.equal(rejected.status, 422);
      assert.ok((await rejected.json()).errors.some(message => message.startsWith('receiverHealth')));
      current = await systemStatus();
      assert.equal(current.processorFresh, false);
      assert.equal(current.processor, null, 'Malformed supplied health must never retain stale healthy state');
      assert.equal((await post({...live, receiverHealth: health})).status, 204);
    }

    const beforeInvalidBase = (await systemStatus()).processor;
    assert.equal((await post({...live, state: 'unknown'})).status, 422);
    assert.deepEqual((await systemStatus()).processor, beforeInvalidBase,
      'Invalid legacy telemetry keeps existing rejection behavior');

    await delay(5100);
    current = await systemStatus();
    assert.equal(current.processorFresh, false);
    assert.equal(current.processor, null, 'Expired telemetry cannot report clear or active overload');
    assert.equal((await post({...live, receiverHealth: active})).status, 204);
    assert.deepEqual((await systemStatus()).processor.receiverHealth, active,
      'Fresh telemetry recovers overload state after an outage');
    assert.deepEqual(fs.readFileSync(filename), original, 'Receiver telemetry must not mutate RF configuration');
    assert.equal((await (await fetch(`${origin}/capture/request`)).json()).recording, false);
    console.log(`Receiver health API ingestion, rejection, freshness and recovery tests passed (${process.platform}).`);
  } finally {
    if (child && child.exitCode === null) {
      child.kill('SIGTERM');
      await new Promise(resolve => child.once('exit', resolve));
    }
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
