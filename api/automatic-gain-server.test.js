'use strict';

// Entirely loopback: real API, real control client and worker-thread IQ parser.
// No receiver hardware or service manager is used.
const assert = require('assert/strict');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {emptyRecord} = require('../html/js/kraken_geometry');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'automatic-gain-api-'));
const filename = path.join(dir, 'config.yml');
const marker = path.join(dir, 'restart');
const journalFault = path.join(dir, 'fail-success-journal');
const preload = path.join(dir, 'preload.cjs');
// Only this disposable child mocks the already separately tested root-receipt
// reader. Production has no environment switch that bypasses trusted ownership.
fs.writeFileSync(preload, `
const fs=require('fs');
require(${JSON.stringify(path.join(__dirname, 'gain-restart.js'))}).installedGainRestartSupported=()=>true;
require(${JSON.stringify(path.join(__dirname, 'sdrplay-startup.js'))}).readSdrplayStartup=()=>{
try{return JSON.parse(fs.readFileSync(${JSON.stringify(marker)},'utf8'));}catch{return null;}
};
const journals=require(${JSON.stringify(path.join(__dirname, 'receiver-journal.js'))});
const createJournal=journals.createReceiverJournal;
journals.createReceiverJournal=(...args)=>{
const journal=createJournal(...args);
return {...journal,write(value){
if(value.state==='synchronized' && value.receipt?.automaticGain?.status==='verified' &&
    fs.existsSync(${JSON.stringify(journalFault)})){
  fs.unlinkSync(${JSON.stringify(journalFault)});
  const error=new Error('Injected final journal failure after configuration save');
  error.code='RECEIVER_JOURNAL_PERSISTENCE_FAILED'; error.status=503; throw error;
}
return journal.write(value);
}};
};
`);
const sockets = new Set();
const commands = [];
const config = yaml.load(fs.readFileSync(path.join(__dirname, '../config/config-kraken.yml'), 'utf8'));
let gain = 14, expectedSavedGain = 14, recovering = false, probes = 0, child, stderr = '', rejectProbe = false;
let gainPhasePending = false;
const status = () => ({settings: {center_freq: config.capture.fc,
  sample_rate: config.capture.fs, gain}, num_channels: 5, max_elements: 5,
  operating_mode: 'coherent', reconfiguring: false, recovering,
  cooldown_active: false}); // Stock Suite omits optional control calibration flags.
function own(socket) { sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket)); }
function send(socket, value) { if (!socket.destroyed) socket.write(JSON.stringify(value) + '\n'); }
const suite = net.createServer(socket => {
  own(socket); send(socket, status()); socket.setEncoding('utf8'); let buffer = '';
  socket.on('data', chunk => {
    buffer += chunk; const lines = buffer.split('\n'); buffer = lines.pop();
    for (const line of lines.filter(Boolean)) {
      const command = JSON.parse(line); commands.push(command);
      assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.heimdall.gain, expectedSavedGain,
        'The file must remain unchanged until verified headroom is available.');
      if (command.command === 'get_scanner_status') {
        send(socket, {status: 'success', enabled: false});
      } else if (command.command === 'set_gain') {
        gain = command.gain; gainPhasePending = true;
        send(socket, {status: 'success', gain}); send(socket, status());
      } else if (command.command === 'reset_lag_compensation') {
        assert.equal(gainPhasePending, false,
          'A full reset must wait for the gain-change phase transition, even when TCP reports idle');
        send(socket, {status: 'success', message: 'Full recalibration (lag + phase) started'});
        recovering = true; send(socket, status());
        setTimeout(() => { recovering = false; send(socket, status()); }, 15);
      } else throw new Error('Unexpected control command ' + line);
    }
  });
});
const iq = net.createServer(socket => {
  own(socket); probes++;
  const phasePending = gainPhasePending;
  const samples = 16384, bytes = 32 + 5 * 8 + 5 * samples * 2;
  const frame = Buffer.alloc(bytes);
  frame.writeUInt32BE(0x4d434851, 0); frame.writeUInt32BE(5, 4);
  frame.writeUInt32BE(samples, 8); frame.writeUInt32BE(phasePending ? 5 : 4, 12);
  for (let channel = 0; channel < 5; channel++) {
    frame.writeFloatLE(config.capture.fc + (rejectProbe ? 6000000 : 0), 32 + channel * 8);
    frame.writeFloatLE(gain, 36 + channel * 8);
  }
  for (let at = 72; at < bytes; at++) frame[at] = gain >= 14 ? (at % 2 ? 255 : 0) : (at % 2 ? 191 : 64);
  if (phasePending) {
    // Expose calibration-in-progress before its asynchronous cleanup. TCP's
    // idle flags deliberately stay false, matching the stock Suite gap.
    socket.write(frame, () => { gainPhasePending = false; });
    return;
  }
  let count = 0;
  function write() {
    if (socket.destroyed) return;
    while (count++ < 40) { if (!socket.write(frame)) { socket.once('drain', write); return; } }
  }
  write();
});
const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let revision;
function request(method, route, body, headers = {}) {
  return new Promise((resolve, reject) => {
    const payload = body === undefined ? null : JSON.stringify(body);
    const req = http.request({host: '127.0.0.1', port: config.network.ports.api, method, path: route,
      headers: {'Content-Type': 'application/json', Origin: `http://127.0.0.1:${config.network.ports.api}`,
        'X-VectorWarp-Intent': 'config-write-v1', 'X-VectorWarp-Receiver-Sync': 'synchronize-v1',
        ...(revision ? {'If-Match': '"' + revision + '"'} : {}), ...headers}}, res => {
      let data = ''; res.on('data', chunk => { data += chunk; });
      res.on('end', () => resolve({status: res.statusCode, body: data ? JSON.parse(data) : null}));
    });
    req.on('error', reject); req.end(payload);
  });
}
async function start() {
  child = spawn(process.execPath, ['--require', preload, path.join(__dirname, 'server.js'), filename], {
    env: {...process.env, BLAH2_CONFIG_RESTART_COMMAND: JSON.stringify([process.execPath, '-e',
      `const fs=require('fs'),crypto=require('crypto');fs.writeFileSync(${JSON.stringify(marker)}, JSON.stringify({state:'complete',inProgress:false,updatedAt:Date.now(),configRevision:crypto.createHash('sha256').update(fs.readFileSync(${JSON.stringify(filename)})).digest('hex')}))`])}, stdio: ['ignore', 'ignore', 'pipe']});
  child.stderr.on('data', chunk => { stderr += chunk; });
  for (let i = 0; i < 100; i++) {
    try { const r = await request('GET', '/api/config/capabilities'); revision = r.body.configRevision; return; }
    catch (_) { await new Promise(resolve => setTimeout(resolve, 25)); }
  }
  throw new Error(stderr);
}
async function stop() { if (child && child.exitCode === null) await new Promise(resolve => { child.once('exit', resolve); child.kill(); }); }
(async () => {
  try {
    await listen(suite); await listen(iq);
    const basePort = 32000 + process.pid % 1000;
    config.network.ip = '127.0.0.1';
    Object.keys(config.network.ports).forEach((key, i) => { config.network.ports[key] = basePort + i; });
    config.truth.adsb.enabled = false;
    config.capture.device.heimdall = {host: '127.0.0.1', port: iq.address().port,
      control_port: suite.address().port, gain: 14, gain_control: 'fixed'};
    config.capture.device.array_geometry = emptyRecord(5);
    fs.writeFileSync(filename, yaml.dump(config));
    await start();
    const proposed = structuredClone(config); proposed.capture.device.heimdall.gain_control = 'measured';
    assert.equal((await request('PUT', '/api/config?restart=false', proposed)).body.code, 'GAIN_CHECK_RESTART_REQUIRED');
    assert.equal(commands.length, 0); assert.equal(probes, 0);
    const result = await request('PUT', '/api/config?restart=true', proposed);
    assert.equal(result.status, 200, JSON.stringify(result.body));
    assert.equal(result.body.receiverSync.automaticGain.status, 'verified');
    assert.equal(result.body.config.capture.device.heimdall.gain, 13);
    assert.equal(probes, 6, 'Two readiness checks plus one transitional capture remain separate from three headroom captures');
    assert.equal(result.body.receiverSync.automaticGain.probes.length, 3);
    assert.equal(result.body.receiverSync.automaticGain.settling[1].transitionObserved, true);
    assert.equal(commands.filter(row => row.command === 'reset_lag_compensation').length, 2);
    assert.equal((await request('GET', '/api/detection')).status, 503, 'No old-pipeline publication before restart');
    for (let i = 0; i < 30 && !fs.existsSync(marker); i++) await new Promise(resolve => setTimeout(resolve, 25));
    assert.ok(fs.existsSync(marker));
    await stop();
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.heimdall.gain, 13);
    await start();
    assert.equal((await request('GET', '/api/detection')).status, 503,
      'An API restart alone must not clear the persisted publication interlock');
    const processor = {input: 'live', receiver: 'Kraken', state: 'live', error: '', file: '',
      frequency: config.capture.fc, sampleRate: config.capture.fs, positionSamples: 0,
      totalSamples: 0, loops: 0, trailingSamples: 0, recording: false, recordingFile: '',
      recordingError: '', recordedSamples: 0};
    assert.equal((await request('POST', '/api/processor/status', processor)).status, 204);
    await new Promise(resolve => {
      const socket = net.createConnection({host: '127.0.0.1', port: config.network.ports.timestamp}, () => {
        socket.end(String(Date.now())); setTimeout(resolve, 25);
      });
    });
    assert.notEqual((await request('GET', '/api/detection')).status, 503,
      'A replacement API with completed restart and fresh matching processor/frame may publish');
    await stop();
    // A successful journal unlocks the replacement API, but a failed new probe
    // restores the previous tuple and leaves processing publication blocked.
    fs.writeFileSync(filename, yaml.dump(config));
    fs.unlinkSync(filename + '.receiver-state.json');
    gain = 14; rejectProbe = true; fs.unlinkSync(marker);
    await start();
    const failed = await request('PUT', '/api/config?restart=true', proposed);
    assert.notEqual(failed.status, 200);
    assert.equal(gain, 14); assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.heimdall.gain, 14);
    assert.equal((await request('GET', '/api/detection')).status, 503);
    assert.equal(fs.existsSync(marker), false);
    await stop(); await start();
    assert.equal((await request('GET', '/api/detection')).status, 503, 'Failed check remains interlocked across API restart');
    await stop();

    // The YAML save can succeed before the final journal write fails. The
    // resulting pending marker must preserve publication blocking while still
    // allowing an explicit Apply to reconcile the recorded uncertain outcome.
    fs.writeFileSync(filename, yaml.dump(config));
    fs.unlinkSync(filename + '.receiver-state.json');
    fs.writeFileSync(journalFault, 'fail once');
    gain = 14; rejectProbe = false;
    await start();
    const journalFailed = await request('PUT', '/api/config?restart=true', proposed);
    assert.equal(journalFailed.status, 503, JSON.stringify(journalFailed.body));
    assert.equal(journalFailed.body.code, 'RECEIVER_JOURNAL_PERSISTENCE_FAILED');
    assert.equal(gain, 13, 'Saved selected gain must not be rolled back after YAML persistence');
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.heimdall.gain, 13);
    expectedSavedGain = 13;
    assert.equal(fs.existsSync(marker), false, 'Failed journal persistence must not request restart');
    const failedJournal = JSON.parse(fs.readFileSync(filename + '.receiver-state.json', 'utf8'));
    assert.equal(failedJournal.reconciliationRequired, true);
    assert.ok(failedJournal.receipt.automaticGain.restartPendingAt);
    await stop(); await start();
    assert.equal((await request('GET', '/api/detection')).status, 503,
      'Failed success-journal write stays interlocked after API restart');
    const recoveryConfig = (await request('GET', '/api/config')).body;
    const recovered = await request('PUT', '/api/config?restart=true', recoveryConfig);
    assert.equal(recovered.status, 200,
      'Pending marker must allow explicit reconciliation: ' + JSON.stringify(recovered.body));
    assert.equal((await request('GET', '/api/detection')).status, 503,
      'Reconciliation cannot publish from the old API process');
    for (let i = 0; i < 30 && !fs.existsSync(marker); i++) await new Promise(resolve => setTimeout(resolve, 25));
    assert.ok(fs.existsSync(marker));
    await stop(); await start();
    assert.equal((await request('GET', '/api/detection')).status, 503);
    assert.equal((await request('POST', '/api/processor/status', processor)).status, 204);
    await new Promise(resolve => {
      const socket = net.createConnection({host: '127.0.0.1', port: config.network.ports.timestamp}, () => {
        socket.end(String(Date.now())); setTimeout(resolve, 25);
      });
    });
    assert.notEqual((await request('GET', '/api/detection')).status, 503,
      'Only the replacement API and fresh processor evidence release recovered publication');
    const reconciled = JSON.parse(fs.readFileSync(filename + '.receiver-state.json', 'utf8'));
    assert.equal(reconciled.reconciliationRequired, false);
    assert.equal(reconciled.receipt.automaticGain.restartPendingAt, undefined);
    console.log('automatic gain API loopback: verified selection, no-restart rejection, publication interlock, rollback, crash and final-journal failure recovery passed');
  } finally {
    await stop(); for (const socket of sockets) socket.destroy(); suite.close(); iq.close();
    fs.rmSync(dir, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); console.error(stderr); process.exitCode = 1; });
