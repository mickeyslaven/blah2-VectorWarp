'use strict';

const assert = require('assert');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');

const source = path.join(__dirname, '..', 'config', 'config-kraken.yml');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'blah2-api-test-'));
const filename = path.join(directory, 'config.yml');
const restartMarker = path.join(directory, 'restart-called');
const config = yaml.load(fs.readFileSync(source, 'utf8'));
const basePort = 20000 + (process.pid % 10000);
const truthPort = basePort + 20;
const suitePort = basePort + 21;
const suiteState = {settings: {center_freq: config.capture.fc, sample_rate: config.capture.fs},
  num_channels: config.capture.device.channel_count, max_elements: 8,
  operating_mode: 'coherent', reconfiguring: false, recovering: false};
// Every production live Kraken save observes Suite. Keep this generic API
// fixture entirely on loopback; never use the default real control endpoint.
const suiteFixture = net.createServer(socket => {
  socket.write(`${JSON.stringify(suiteState)}\n`);
  socket.setEncoding('utf8');
  let buffer = '';
  socket.on('data', chunk => {
    buffer += chunk;
    const lines = buffer.split('\n'); buffer = lines.pop();
    for (const line of lines.filter(Boolean)) {
      const command = JSON.parse(line);
      if (command.command === 'set_frequency') {
        suiteState.settings.center_freq = command.frequency;
        socket.write(`${JSON.stringify({status: 'success', frequency: command.frequency})}\n`);
      } else if (command.command === 'set_num_elements') {
        suiteState.num_channels = command.num_elements;
        socket.write(`${JSON.stringify({status: 'success', num_elements: command.num_elements})}\n`);
      } else throw new Error('Unexpected simulated Suite command');
      socket.write(`${JSON.stringify(suiteState)}\n`);
    }
  });
});
suiteFixture.listen(suitePort, '127.0.0.1');
config.capture.device.heimdall.host = '127.0.0.1';
config.capture.device.heimdall.control_port = suitePort;
const truthRequests = [];
const truthFixture = http.createServer((req, res) => {
  truthRequests.push(req.url);
  const url = new URL(req.url, 'http://fixture.invalid');
  res.setHeader('Content-Type', 'application/json');
  if (url.pathname === '/receiver/data/aircraft.json')
    return res.end(JSON.stringify({now: Date.now() / 1000, aircraft: [{hex: 'abc123', flight: 'TEST', lat: 1, lon: 2, alt_geom: 1000, seen_pos: 0}]}));
  res.writeHead(404); res.end('{}');
});
truthFixture.listen(truthPort, '127.0.0.1');
config.truth.adsb = {enabled: true, tar1090: `127.0.0.1:${truthPort}/receiver`};
config.network.ip = '127.0.0.1';
Object.keys(config.network.ports).forEach((key, index) => {
  config.network.ports[key] = basePort + index;
});
fs.writeFileSync(filename, yaml.dump(config));

const child = spawn(process.execPath, [path.join(__dirname, 'server.js'), filename], {
  env: {...process.env, BLAH2_CONFIG_RESTART_COMMAND: JSON.stringify([
    process.execPath, '-e',
    `const fs=require('fs');fs.writeFileSync(${JSON.stringify(restartMarker)}, 'ok');if(fs.readFileSync(${JSON.stringify(filename)},'utf8').includes('RESTART_FAIL'))process.exit(23)`
  ]), BLAH2_RECEIVER_TYPES: 'Kraken'},
  stdio: ['ignore', 'ignore', 'pipe']
});
let childError = '';
let revision = null;
child.stderr.on('data', chunk => { childError += chunk; });

function requestOnce(method, pathname, body, headers = {}) {
  return new Promise((resolve, reject) => {
    const payload = body === undefined ? null : JSON.stringify(body);
    const req = http.request({
      hostname: '127.0.0.1',
      port: config.network.ports.api,
      path: pathname,
      method,
      headers: {
        ...(method === 'PUT' ? {'X-VectorWarp-Receiver-Sync': 'synchronize-v1'} : {}),
        ...(revision ? {'If-Match': `"${revision}"`} : {}),
        ...(payload ? {'Content-Type': 'application/json',
          'Content-Length': Buffer.byteLength(payload)} : {}),
        ...headers
      }
    }, response => {
      let text = '';
      response.setEncoding('utf8');
      response.on('data', chunk => { text += chunk; });
      response.on('end', () => resolve({
        status: response.statusCode,
        headers: response.headers,
        body: text ? JSON.parse(text) : null
      }));
    });
    req.on('error', reject);
    if (payload) req.write(payload);
    req.end();
  });
}

async function request(method, pathname, body, headers = {}) {
  try {
    return await requestOnce(method, pathname, body, headers);
  } catch (error) {
    if (error.code !== 'ECONNRESET') throw error;
    await new Promise(resolve => setTimeout(resolve, 40));
    return requestOnce(method, pathname, body, headers);
  }
}

async function waitForServer() {
  for (let attempt = 0; attempt < 50; attempt += 1) {
    try {
      const response = await request('GET', '/api/config/capabilities');
      if (response.status === 200) return response;
    } catch (_) {
      await new Promise(resolve => setTimeout(resolve, 40));
    }
  }
  throw new Error(`API did not start: ${childError}`);
}

async function waitForSystemStatus(predicate, message) {
  for (let attempt = 0; attempt < 50; attempt += 1) {
    const status = (await request('GET', '/api/system/status')).body;
    if (predicate(status)) return status;
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  throw new Error(message);
}

function closeSocket(socket) {
  return new Promise(resolve => { socket.once('close', resolve); socket.end(); });
}

(async () => {
  try {
    const capabilities = await waitForServer();
    revision = capabilities.body.configRevision;
    assert.equal(capabilities.body.editable, true);
    assert.equal(capabilities.body.restartAvailable, true);
    // Query parsing is exposed by every HTTP route. These hostile-looking
    // inputs must stay data, never poison objects or take the API down.
    for (const query of [
      '?__proto__[polluted]=yes',
      '?constructor[prototype][polluted]=yes',
      `?${'deep'.repeat(200)}[value]=1`
    ]) {
      const response = await request('GET', `/api/system/status${query}`);
      assert.equal(response.status, 200, `Query must not break status: ${query.slice(0, 40)}`);
    }
    assert.equal({}.polluted, undefined);
    const busy = JSON.parse(JSON.stringify(config));
    busy.network.ports.map = truthPort;
    for (const method of ['POST', 'PUT']) {
      const rejected = await request(method, method === 'POST' ? '/api/config/validate' : '/api/config?restart=true', busy);
      assert.equal(rejected.status, 422);
      assert.ok(rejected.body.errors.some(error => error.includes('network.ports.map is already in use')));
    }
    assert.equal(fs.existsSync(restartMarker), false, 'Busy ports must not trigger a restart');
    const fileMode = fs.statSync(filename).mode & 0o777;
    fs.chmodSync(filename, 0o444);
    try {
      assert.equal((await request('GET', '/api/config/capabilities')).body.editable, false);
      assert.equal((await request('PUT', '/api/config?restart=true', config)).status, 503);
    } finally { fs.chmodSync(filename, fileMode); }
    assert.deepEqual(capabilities.body.deviceProfiles.map(item => item.type).sort(),
      ['HackRF', 'Kraken', 'RspDuo', 'Usrp']);
    assert.deepEqual(capabilities.body.compiledLiveTypes, ['Kraken']);
    assert.equal(capabilities.body.deviceProfiles.find(item => item.type === 'Kraken').liveAvailable, true);
    for (const type of ['RspDuo', 'Usrp', 'HackRF'])
      assert.equal(capabilities.body.deviceProfiles.find(item => item.type === type).liveAvailable, false);
    const aircraft = await request('GET', '/api/adsb');
    assert.equal(aircraft.status, 200);
    assert.equal(aircraft.body.aircraft[0].hex, 'abc123');
    const dd = await request('GET', '/api/adsb/delay-doppler');
    assert.equal(dd.status, 200);
    assert.deepEqual(dd.body, {}, 'One raw position is motion warmup');
    const legacyLink = await request('GET', '/api/adsb2dd');
    assert.equal(legacyLink.body.url, '/api/adsb/delay-doppler');
    const feeds = await request('GET', '/api/adsb/status');
    assert.equal(feeds.body.online, true);
    assert.equal((await request('GET', '/api/system/status')).body.adsbEnabled, true);
    assert.ok(truthRequests.some(url => url === '/receiver/data/aircraft.json'));

    const replayStatus = {input: 'replay', receiver: 'Kraken', state: 'playing',
      error: '', file: '/tmp/pass.mchq', sampleRate: 2400000, frequency: 527000000,
      positionSamples: 12, totalSamples: 24, loops: 0, trailingSamples: 0,
      recording: false, recordingFile: '', recordingError: '', recordedSamples: 0};
    assert.equal((await request('POST', '/api/processor/status', replayStatus)).status, 204);
    const truthRequestsBeforeReplay = truthRequests.length;
    for (const route of ['/api/adsb', '/api/adsb/delay-doppler', '/api/adsb2dd', '/api/adsb/status']) {
      const disabled = await request('GET', route);
      assert.equal(disabled.status, 200);
      assert.equal(disabled.body.disabledForReplay, true, `${route} must disclose replay suppression`);
      assert.equal(disabled.body.enabled, false);
    }
    assert.equal(truthRequests.length, truthRequestsBeforeReplay,
      'Replay must not fetch live aircraft or delay–Doppler truth');
    let system = (await request('GET', '/api/system/status')).body;
    assert.equal(system.processorFresh, true);
    assert.deepEqual(system.processor.input, 'replay');
    assert.equal(system.processor.state, 'playing');
    assert.equal(system.acceleration, null);
    assert.equal(system.clutterAcceleration, null, 'No timing must not imply a selected clutter backend');
    assert.equal((await request('POST', '/api/processor/status', {
      ...replayStatus, state: 'complete', positionSamples: 24
    })).status, 204);
    system = (await request('GET', '/api/system/status')).body;
    assert.equal(system.processorFresh, true, 'EOF status stays visible while telemetry is fresh');
    assert.equal(system.processor.state, 'complete');
    assert.equal((await request('POST', '/api/processor/status', {
      ...replayStatus, state: 'error', error: 'Recording truncated\nat packet 7'
    })).status, 204);
    system = (await request('GET', '/api/system/status')).body;
    assert.equal(system.processor.state, 'error');
    assert.equal(system.processor.error, 'Recording truncated at packet 7');
    assert.equal((await request('POST', '/api/processor/status',
      {...replayStatus, state: 'error', error: ''})).status, 422);
    assert.equal((await request('POST', '/api/processor/status', replayStatus,
      {Origin: 'https://unrelated.example'})).status, 403);
    assert.equal((await request('GET', '/capture/status')).body.supported, false,
      'Recording must be disabled while replay telemetry is fresh');
    assert.equal((await request('POST', '/api/processor/status', {
      ...replayStatus, input: 'live', state: 'live', file: ''
    })).status, 204);
    const restoredTruth = await request('GET', '/api/adsb');
    assert.equal(restoredTruth.status, 200);
    assert.equal(restoredTruth.body.aircraft[0].hex, 'abc123');
    assert.ok(truthRequests.length > truthRequestsBeforeReplay,
      'Live truth resumes only after live processor telemetry');

    const idleCapture = await request('GET', '/capture/status');
    assert.equal(idleCapture.status, 200);
    assert.equal(idleCapture.body.recording, false);
    assert.equal(idleCapture.body.elapsedSeconds, 0);
    assert.equal(idleCapture.body.available, false);
    assert.deepEqual((await request('GET', '/capture/request')).body, {recording: false, revision: 0});
    assert.equal((await request('GET', '/capture/toggle')).status, 409);
    // Histories update from their own stream, even before the separate frame
    // marker arrives. Exercise real TCP -> stash -> HTTP in this isolated API.
    for (const [stream, endpoint, frame] of [
      ['map', '/stash/map', {timestamp: 1000, nRows: 1, delay: [1], doppler: [2], data: [[3]]}],
      ['detection', '/stash/detection', {timestamp: 1000, delay: [1], doppler: [2], snr: [3]}],
      ['iqdata', '/stash/iqdata', {timestamp: 1000, frequency: [100], spectrum: [3]}],
      ['timing', '/stash/timing', {timestamp: 1000, cpi: 10}]
    ]) {
      await new Promise((resolve, reject) => {
        const socket = net.createConnection({host: '127.0.0.1', port: config.network.ports[stream]},
          () => socket.end(JSON.stringify(frame)));
        socket.on('close', resolve);
        socket.on('error', reject);
      });
      const history = (await request('GET', endpoint)).body;
      assert.equal(history.frameTimestamp ?? history.timestamp, 1000, `${stream} must update without a polling timer`);
    }
    const ambiguityBackend = {requested: 'auto', active: 'vulkan', state: 'ready',
      device: 'simulated GPU', reason: '', cpuMs: 8, gpuMs: 2};
    for (const [index, clutterBackend] of [
      {...ambiguityBackend, cpuMs: 14, gpuMs: 4, gpuExecuted: true, cpuExecuted: false},
      {...ambiguityBackend, active: 'cpu', state: 'fallback', reason: 'simulated clutter-only fallback',
        gpuExecuted: false, cpuExecuted: true},
      null
    ].entries()) {
      const frame = {timestamp: 1001 + index, cpi: 10, acceleration: ambiguityBackend,
        ...(clutterBackend ? {clutterAcceleration: clutterBackend} : {})};
      const socket = await new Promise((resolve, reject) => {
        const connection = net.createConnection({host: '127.0.0.1', port: config.network.ports.timing}, () => {
          connection.write(JSON.stringify(frame)); resolve(connection);
        });
        connection.on('error', reject);
      });
      const reported = await waitForSystemStatus(status => status.acceleration?.active === 'vulkan',
        'Timing frame was not observed while its socket remained connected');
      assert.deepEqual(reported.acceleration, ambiguityBackend, 'Existing ambiguity telemetry is unchanged');
      assert.deepEqual(reported.clutterAcceleration, clutterBackend,
        'Clutter state and execution flags propagate independently; older timing frames clear the field');
      await closeSocket(socket);
      assert.equal((await request('GET', '/api/system/status')).body.acceleration, null,
        'A closed timing stream must not leave GPU qualification telemetry current');
    }
    const reconnectedTimingSocket = await new Promise((resolve, reject) => {
      const socket = net.createConnection({host: '127.0.0.1', port: config.network.ports.timing}, () => resolve(socket));
      socket.on('error', reject);
    });
    assert.equal((await request('GET', '/api/system/status')).body.acceleration, null,
      'A new timing connection cannot reuse telemetry from the previous generation');
    await closeSocket(reconnectedTimingSocket);
    // Test-only frame marker; never touches a physical receiver.
    const timestampSocket = await new Promise((resolve, reject) => {
      const socket = net.createConnection({host: '127.0.0.1', port: config.network.ports.timestamp}, () => {
        socket.write(String(Date.now())); resolve(socket);
      });
      socket.on('error', reject);
    });
    await waitForSystemStatus(status => status.lastFrameAt !== null,
      'Timestamp frame was not observed while its socket remained connected');
    const startedCapture = await request('GET', '/capture/toggle');
    assert.equal(startedCapture.body.requested, true);
    assert.equal(startedCapture.body.recording, false);
    assert.equal(startedCapture.body.revision, 1);
    assert.deepEqual((await request('GET', '/capture/request')).body, {recording: true, revision: 1});
    assert.ok(Number.isFinite(startedCapture.body.startedAt));
    const processorCaptureFlag = await request('GET', '/capture');
    assert.equal(processorCaptureFlag.body, true,
      'Legacy processor polling contract must remain a JSON boolean');
    const liveRecording = {...replayStatus, input: 'live', state: 'live', file: '',
      recording: true, recordingRequestId: 1, recordingFile: '/tmp/new.blah2', recordedSamples: 128};
    assert.equal((await request('POST', '/api/processor/status', liveRecording)).status, 204);
    const activeCapture = await request('GET', '/capture/status');
    assert.equal(activeCapture.body.recording, true);
    assert.equal(activeCapture.body.requested, true);
    assert.equal(activeCapture.body.acknowledged, true);
    assert.equal(activeCapture.body.recordingFile, '/tmp/new.blah2');
    assert.equal(activeCapture.body.recordedSamples, 128);
    const stoppedCapture = await request('GET', '/capture/toggle');
    assert.equal(stoppedCapture.body.requested, false);
    assert.equal(stoppedCapture.body.revision, 2);
    assert.equal(stoppedCapture.body.recording, true);
    assert.equal(stoppedCapture.body.startedAt, null);
    assert.equal((await request('POST', '/api/processor/status', {
      ...liveRecording, recording: false, recordingRequestId: 2, recordingFile: ''
    })).status, 204);
    assert.equal((await request('GET', '/capture/status')).body.acknowledged, true);
    assert.equal((await request('GET', '/capture/toggle')).body.requested, true);
    assert.equal((await request('POST', '/api/processor/status', {
      ...liveRecording, recording: false, recordingRequestId: 3, recordingError: 'Disk full\nrecording stopped'
    })).status, 204);
    const failedCapture = await request('GET', '/capture/status');
    assert.equal(failedCapture.body.requested, false,
      'A writer failure must clear the request instead of leaving Starting recording forever');
    assert.equal(failedCapture.body.recordingError, 'Disk full recording stopped');
    await closeSocket(timestampSocket);
    assert.equal((await request('GET', '/api/system/status')).body.lastFrameAt, null,
      'A closed timestamp stream must not leave a frame current');
    const reconnectedTimestampSocket = await new Promise((resolve, reject) => {
      const socket = net.createConnection({host: '127.0.0.1', port: config.network.ports.timestamp}, () => resolve(socket));
      socket.on('error', reject);
    });
    assert.equal((await request('GET', '/api/system/status')).body.lastFrameAt, null,
      'A new timestamp connection cannot reuse a frame from the previous generation');
    await closeSocket(reconnectedTimestampSocket);

    const foreign = await request('PUT', '/api/config?restart=true', config, {
      Origin: 'https://unrelated.example'
    });
    assert.equal(foreign.status, 403);

    const invalid = JSON.parse(JSON.stringify(config));
    invalid.unexpected = true;
    const validationRejected = await request(
      'POST', '/api/config/validate', invalid);
    assert.equal(validationRejected.status, 422);
    assert.equal(validationRejected.body.valid, false);
    const rejected = await request('PUT', '/api/config?restart=true', invalid);
    assert.equal(rejected.status, 422);

    const validationAccepted = await request(
      'POST', '/api/config/validate', config);
    assert.equal(validationAccepted.status, 200);
    assert.equal(validationAccepted.body.valid, true);

    const updated = JSON.parse(JSON.stringify(config));
    updated.process.data.cpi = 0.25;
    updated.process.performance.fft_threads = 0;
    const liveTimingSocket = await new Promise((resolve, reject) => {
      const socket = net.createConnection({host: '127.0.0.1', port: config.network.ports.timing}, () => {
        socket.write(JSON.stringify({timestamp: 2001, cpi: 10, acceleration: ambiguityBackend})); resolve(socket);
      });
      socket.on('error', reject);
    });
    const liveTimestampSocket = await new Promise((resolve, reject) => {
      const socket = net.createConnection({host: '127.0.0.1', port: config.network.ports.timestamp}, () => {
        socket.write(String(Date.now())); resolve(socket);
      });
      socket.on('error', reject);
    });
    const liveStatus = await waitForSystemStatus(status => status.acceleration?.active === 'vulkan' &&
      status.lastFrameAt !== null, 'Live timing and timestamp streams were not observed before restart');
    assert.deepEqual(liveStatus.acceleration, ambiguityBackend,
      'Live streams make their own current telemetry visible before a restart');
    const accepted = await request('PUT', '/api/config?restart=true', updated, {
      Origin: `http://127.0.0.1:49152`
    });
    assert.equal(accepted.status, 200);
    assert.equal(accepted.body.restarting, true);
    revision = accepted.body.revision;
    for (let attempt = 0;
      attempt < 25 && !fs.existsSync(restartMarker); attempt += 1)
      await new Promise(resolve => setTimeout(resolve, 20));
    assert.ok(fs.existsSync(restartMarker),
      'Configured restart command was not executed');
    const afterRestart = (await request('GET', '/api/system/status')).body;
    assert.equal(afterRestart.lastFrameAt, null,
      'Restart invalidates the old timestamp generation before a new connection reports frames');
    assert.equal(afterRestart.acceleration, null,
      'Restart invalidates the old timing generation before it can qualify GPU setup');
    await Promise.all([closeSocket(liveTimingSocket), closeSocket(liveTimestampSocket)]);
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).process.data.cpi,
      0.25);
    assert.equal((await request('GET', '/api/runtime/config')).body.process.data.cpi,
      config.process.data.cpi, 'Saved timing must not impersonate the still-running configuration');
    assert.equal((await request('GET', '/api/config')).body.process.data.cpi, 0.25);
    assert.ok(fs.existsSync(`${filename}.bak`));
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).process.performance.fft_threads, 0);

    const stale = await request('PUT', '/api/config?restart=false', updated, {'If-Match': '"stale"'});
    assert.equal(stale.status, 409);
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).process.data.cpi, 0.25);

    let previous = updated;
    for (const profile of capabilities.body.deviceProfiles) {
      const switched = JSON.parse(JSON.stringify(previous));
      switched.capture.fs = profile.sampleRate;
      switched.capture.device = profile.device;
      if (profile.type === 'Kraken') switched.capture.device.heimdall = {...profile.device.heimdall,
        host: '127.0.0.1', control_port: suitePort};
      switched.capture.replay.state = profile.liveAvailable === false;
      if (profile.type === 'HackRF') switched.capture.device.serial = ['0001', '0002'];
      for (const key of ['performance', 'reference_synthesis']) {
        if (profile.process?.[key] !== undefined)
          switched.process[key] = profile.process[key];
        else
          delete switched.process[key];
      }
      const deviceAccepted = await request(
        'PUT', '/api/config?restart=false', switched);
      assert.equal(deviceAccepted.status, 200, `${profile.type}: ${JSON.stringify(deviceAccepted.body)}`);
      revision = deviceAccepted.body.revision;
      assert.equal(yaml.load(fs.readFileSync(filename, 'utf8'))
        .capture.device.type, profile.type);
      previous = switched;
    }
    const unsupportedLive = JSON.parse(JSON.stringify(previous));
    unsupportedLive.capture.replay.state = false;
    const liveRejected = await request('PUT', '/api/config?restart=false', unsupportedLive);
    assert.equal(liveRejected.status, 422);
    assert.match(liveRejected.body.errors.join(' '), /unavailable for live capture/);
    const onDisk = yaml.load(fs.readFileSync(filename, 'utf8'));
    onDisk.location.rx.name = 'Edited outside browser';
    fs.writeFileSync(filename, yaml.dump(onDisk));
    const external = await request('GET', '/api/config');
    assert.equal(external.body.location.rx.name, 'Edited outside browser');
    assert.notEqual(external.headers.etag, `"${revision}"`);
    const conflict = await request('PUT', '/api/config?restart=false', previous);
    assert.equal(conflict.status, 409);
    revision = external.headers.etag.replace(/^"|"$/g, '');
    onDisk.location.tx.name = 'RESTART_FAIL';
    const failedRestart = await request('PUT', '/api/config?restart=true', onDisk);
    assert.equal(failedRestart.status, 200);
    let status;
    for (let i = 0; i < 60; i++) {
      status = (await request('GET', '/api/system/status')).body;
      if (status.restart.state === 'failed') break;
      await new Promise(resolve => setTimeout(resolve, 25));
    }
    assert.equal(status.restart.state, 'failed');
    assert.match(status.restart.message, /exit 23/);
    assert.equal((await request('GET', '/api/config')).status, 200,
      'Failed restart must not make saved settings inaccessible');
    console.log('Configuration API integration tests passed.');
  } finally {
    child.kill('SIGTERM');
    truthFixture.close();
    suiteFixture.close();
    fs.rmSync(directory, {recursive: true});
  }
})().catch(error => {
  console.error(error);
  if (childError) console.error(childError);
  process.exitCode = 1;
});
