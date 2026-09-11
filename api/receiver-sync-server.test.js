'use strict';

// This test starts only an isolated API child and a loopback TCP simulation of
// the inspected Suite V2 protocol. It never discovers or opens receiver USB
// devices, invokes systemd, or runs a package manager.
const assert = require('assert');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {emptyRecord} = require('../html/js/kraken_geometry');

const clone = value => JSON.parse(JSON.stringify(value));
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-receiver-sync-'));
const filename = path.join(directory, 'config.yml');
const original = yaml.load(fs.readFileSync(path.join(
  __dirname, '..', 'config', 'config-kraken.yml'), 'utf8'));
const commands = [];
const diskAtCommand = [];
const sockets = new Set();
let suiteConnections = 0;
const suiteState = {frequency: original.capture.fc, sampleRate: original.capture.fs,
  channels: original.capture.device.channel_count, maximum: 8,
  reconfiguring: false, mode: 'coherent'};
let suiteBehavior = 'confirm';

function suiteStatus() {
  return {settings: {center_freq: suiteState.frequency,
    sample_rate: suiteState.sampleRate, gain: 20},
  num_channels: suiteState.channels, max_elements: suiteState.maximum,
  reconfiguring: suiteState.reconfiguring, recovering: false,
  cooldown_active: false, operating_mode: suiteState.mode};
}

function send(socket, value) {
  if (!socket.destroyed) socket.write(`${JSON.stringify(value)}\n`);
}

const suite = net.createServer(socket => {
  suiteConnections += 1;
  sockets.add(socket);
  socket.setEncoding('utf8');
  send(socket, suiteStatus());
  let buffer = '';
  socket.on('data', chunk => {
    buffer += chunk;
    const lines = buffer.split('\n');
    buffer = lines.pop();
    for (const line of lines) {
      if (!line.trim()) continue;
      const command = JSON.parse(line);
      commands.push(command);
      diskAtCommand.push(yaml.load(fs.readFileSync(filename, 'utf8')));
      if (suiteBehavior === 'reject-frequency' && command.command === 'set_frequency') {
        suiteBehavior = 'confirm';
        send(socket, {status: 'error', message: 'simulated frequency rejection after count change'});
        continue;
      }
      if (suiteBehavior === 'reject') {
        suiteBehavior = 'confirm';
        send(socket, {status: 'error', message: 'simulated tuner rejection'});
        continue;
      }
      if (command.command === 'set_frequency') {
        send(socket, {status: 'success', frequency: command.frequency});
        if (suiteBehavior === 'ack-only') {
          suiteBehavior = 'confirm';
          send(socket, suiteStatus());
          continue;
        }
        setTimeout(() => {
          suiteState.frequency = command.frequency;
          if (suiteBehavior === 'external-edit') {
            suiteBehavior = 'confirm';
            const externallyEdited = yaml.load(fs.readFileSync(filename, 'utf8'));
            externallyEdited.location.rx.name = 'Concurrent external edit';
            fs.writeFileSync(filename, yaml.dump(externallyEdited));
          }
          send(socket, suiteStatus());
        }, 12);
      } else if (command.command === 'set_num_elements') {
        send(socket, {status: 'success', num_elements: command.num_elements,
          message: 'reconfiguration started'});
        suiteState.reconfiguring = true;
        send(socket, suiteStatus());
        setTimeout(() => {
          suiteState.channels = command.num_elements;
          suiteState.reconfiguring = false;
          send(socket, suiteStatus());
        }, 12);
      } else {
        send(socket, {status: 'error', message: 'unsupported command'});
      }
    }
  });
  socket.on('close', () => sockets.delete(socket));
});

let child;
let childError = '';

function startApi() {
  child = spawn(process.execPath, [path.join(__dirname, 'server.js'), filename], {
    env: {...process.env,
      BLAH2_RECEIVER_TYPES: 'Kraken,RspDuo,Usrp,HackRF',
      BLAH2_RECEIVER_STATUS_TIMEOUT_MS: '250',
      BLAH2_RECEIVER_READBACK_TIMEOUT_MS: '250',
      // Harmless disposable child: never invoke a service manager in this test.
      BLAH2_CONFIG_RESTART_COMMAND: JSON.stringify([process.execPath, '-e', 'process.exit(0)'])},
    stdio: ['ignore', 'ignore', 'pipe']
  });
  child.stderr.on('data', chunk => { childError += chunk; });
}

async function restartApi(apiPort) {
  await new Promise(resolve => {
    child.once('exit', resolve);
    child.kill('SIGTERM');
  });
  startApi();
  return waitForApi(apiPort);
}

function request(apiPort, method, pathname, body, revision, headers = {}) {
  return new Promise((resolve, reject) => {
    const payload = body === undefined ? null : JSON.stringify(body);
    const req = http.request({hostname: '127.0.0.1', port: apiPort,
      path: pathname, method, headers: {
        ...(revision ? {'If-Match': `"${revision}"`} : {}),
        ...(payload ? {'Content-Type': 'application/json',
          'Content-Length': Buffer.byteLength(payload)} : {}),
        ...headers
      }}, response => {
      let text = '';
      response.setEncoding('utf8');
      response.on('data', chunk => { text += chunk; });
      response.on('end', () => resolve({status: response.statusCode,
        headers: response.headers, body: text ? JSON.parse(text) : null}));
    });
    req.on('error', reject);
    if (payload) req.write(payload);
    req.end();
  });
}

async function waitForApi(apiPort) {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    try {
      const response = await request(apiPort, 'GET', '/api/config/capabilities');
      if (response.status === 200) return response;
    } catch (_) { /* The isolated child has not bound yet. */ }
    await new Promise(resolve => setTimeout(resolve, 25));
  }
  throw new Error(`Isolated API did not start: ${childError}`);
}

function withSixChannels(config) {
  const value = clone(config);
  value.capture.device.channel_count = 6;
  value.capture.device.surveillance_channels = [0, 1, 2, 3, 4, 5];
  value.process.reference_synthesis.channels = [0, 1, 2, 3, 4, 5];
  return value;
}

(async () => {
  try {
    await new Promise((resolve, reject) => {
      suite.once('error', reject);
      suite.listen(0, '127.0.0.1', resolve);
    });
    const apiPort = 21000 + (process.pid % 1000);
    original.network.ip = '127.0.0.1';
    for (const [index, name] of Object.keys(original.network.ports).entries())
      original.network.ports[name] = apiPort + index;
    original.capture.device.heimdall.control_port = suite.address().port;
    original.capture.device.array_geometry = emptyRecord(original.capture.device.channel_count);
    original.capture.device.array_geometry.elements.forEach((element, index) => {
      element.daq_channel = index; element.position = [index * .1, index % 2 * .1, 0];
    });
    fs.writeFileSync(filename, yaml.dump(original));

    startApi();
    const capabilities = await waitForApi(apiPort);
    let revision = capabilities.body.configRevision;
    assert.equal(capabilities.body.receiverSynchronization.intentHeader,
      'X-VectorWarp-Receiver-Sync');
    const intent = {Origin: `http://127.0.0.1:${apiPort}`,
      'X-VectorWarp-Receiver-Sync': 'synchronize-v1'};

    const changed = withSixChannels(original);
    changed.capture.fc += 1000000;
    const accepted = await request(apiPort, 'PUT', '/api/config?restart=false',
      changed, revision, intent);
    assert.equal(accepted.status, 200, JSON.stringify(accepted.body));
    assert.equal(accepted.body.receiverSync.status, 'synchronized');
    assert.equal(accepted.body.receiverSync.configPersisted, true);
    assert.deepEqual(accepted.body.receiverSync.operations.map(item => item.operation),
      ['set_num_elements', 'set_frequency']);
    assert.deepEqual(commands, [
      {command: 'set_num_elements', num_elements: 6},
      {command: 'set_frequency', frequency: changed.capture.fc}
    ]);
    assert.ok(diskAtCommand.every(value => value.capture.fc === original.capture.fc &&
      value.capture.device.channel_count === original.capture.device.channel_count),
    'YAML must not change before command acknowledgement and status readback');
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.fc,
      changed.capture.fc);
    revision = accepted.body.revision;

    // Save operator statements, then leave a count operation applied while a
    // later frequency command fails. YAML must stay intact on that failure.
    changed.capture.device.array_geometry.mapping_confirmed = true;
    changed.capture.device.array_geometry.geometry_confirmed = true;
    const statements = await request(apiPort, 'PUT', '/api/config?restart=false', changed, revision, intent);
    assert.equal(statements.status, 200, JSON.stringify(statements.body));
    revision = statements.body.revision;
    assert.equal(statements.body.config.capture.device.array_geometry.mapping_confirmed, true);
    const failedNext = clone(changed);
    failedNext.capture.device.channel_count = 5;
    failedNext.capture.device.surveillance_channels = [0, 1, 2, 3, 4];
    failedNext.process.reference_synthesis.channels = [0, 1, 2, 3, 4];
    failedNext.capture.fc += 2000000;
    suiteBehavior = 'reject-frequency';
    const rejectedSecond = await request(apiPort, 'PUT', '/api/config?restart=false', failedNext, revision, intent);
    assert.equal(rejectedSecond.status, 409);
    assert.equal(rejectedSecond.body.receiverSync.status, 'partial');
    assert.equal(suiteState.channels, 5);
    let unchanged = yaml.load(fs.readFileSync(filename, 'utf8'));
    assert.equal(unchanged.capture.device.channel_count, 6);
    assert.equal(unchanged.capture.device.array_geometry.mapping_confirmed, true, 'Failed receiver transaction must not rewrite YAML');
    const uncertain = await request(apiPort, 'GET', '/api/system/status');
    assert.equal(uncertain.body.receiverSynchronization.reconciliationRequired, true);
    const beforePending = suiteConnections;
    const blockedPending = await request(apiPort, 'PUT', '/api/config?mode=pending&restart=false', unchanged, revision,
      {...intent, 'X-VectorWarp-Receiver-Sync': 'save-pending-v1'});
    assert.equal(blockedPending.status, 409);
    assert.equal(blockedPending.body.code, 'RECEIVER_RECONCILIATION_REQUIRED');
    assert.equal(suiteConnections, beforePending, 'Blocked pending save opens no receiver connection');
    assert.deepEqual((await request(apiPort, 'GET', '/api/system/status')).body.receiverSynchronization,
      uncertain.body.receiverSynchronization, 'Pending save must preserve the exact unresolved receipt');
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.channel_count, 6);
    suiteState.sampleRate = 2000000;
    const stillUncertain = await request(apiPort, 'PUT', '/api/config?restart=false', unchanged, revision, intent);
    assert.equal(stillUncertain.status, 409);
    assert.equal((await request(apiPort, 'GET', '/api/system/status')).body.receiverSynchronization.reconciliationRequired, true,
      'A failed reconciliation attempt must not erase the unresolved receiver outcome');
    // Kill and recreate the actual isolated API child: the uncertain receiver
    // outcome must survive and unchanged YAML still needs full readback.
    const restarted = await restartApi(apiPort);
    assert.equal(restarted.body.configRevision, revision);
    assert.equal((await request(apiPort, 'GET', '/api/system/status')).body.receiverSynchronization.reconciliationRequired, true,
      'Unresolved receiver transactions must survive API restart');
    const pendingAfterRestart = await request(apiPort, 'PUT', '/api/config?mode=pending&restart=false', unchanged, revision,
      {...intent, 'X-VectorWarp-Receiver-Sync': 'save-pending-v1'});
    assert.equal(pendingAfterRestart.body.code, 'RECEIVER_RECONCILIATION_REQUIRED');
    const beforeRestartReadback = suiteConnections;
    const afterRestartMismatch = await request(apiPort, 'PUT', '/api/config?restart=false', unchanged, revision, intent);
    assert.equal(afterRestartMismatch.status, 409, JSON.stringify(afterRestartMismatch.body));
    assert.equal(afterRestartMismatch.body.code, 'KRAKEN_SAMPLE_RATE_MISMATCH');
    assert.equal(suiteConnections, beforeRestartReadback + 1);
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.device.array_geometry.mapping_confirmed, true,
      'Failed post-restart readback must leave YAML untouched');
    suiteState.sampleRate = original.capture.fs;
    // Resaving the unchanged saved target must repair/read back the receiver,
    // despite the fact that the ordinary config diff is empty.
    const beforeRepair = commands.length;
    const repaired = await request(apiPort, 'PUT', '/api/config?restart=false', unchanged, revision, intent);
    assert.equal(repaired.status, 200, JSON.stringify(repaired.body));
    assert.deepEqual(commands.slice(beforeRepair), [{command: 'set_num_elements', num_elements: 6}]);
    assert.equal(repaired.body.config.capture.device.array_geometry.mapping_confirmed, false);
    assert.equal(repaired.body.config.capture.device.array_geometry.geometry_confirmed, false);
    assert.equal(repaired.body.receiverSync.hardwareVerified, false);
    revision = repaired.body.revision;

    // Even without a volatile failure flag, every unchanged live save must
    // observe current state. Matched state sends no commands and cannot turn
    // an operator statement into physical verification or erase it by itself.
    const matchedTarget = clone(repaired.body.config);
    matchedTarget.capture.device.array_geometry.mapping_confirmed = true;
    matchedTarget.capture.device.array_geometry.geometry_confirmed = true;
    const beforeMatchedConnections = suiteConnections;
    const beforeMatchedCommands = commands.length;
    const matchedSave = await request(apiPort, 'PUT', '/api/config?restart=false', matchedTarget, revision, intent);
    assert.equal(matchedSave.status, 200, JSON.stringify(matchedSave.body));
    assert.equal(matchedSave.body.receiverSync.status, 'already-matched');
    assert.equal(matchedSave.body.receiverSync.hardwareVerified, false);
    assert.equal(suiteConnections, beforeMatchedConnections + 1);
    assert.equal(commands.length, beforeMatchedCommands);
    assert.equal(matchedSave.body.config.capture.device.array_geometry.mapping_confirmed, true);
    assert.equal(matchedSave.body.config.capture.device.array_geometry.geometry_confirmed, true);
    revision = matchedSave.body.revision;
    for (const [field, invalid, code] of [['mode', 'wideband', 'KRAKEN_MODE_MISMATCH'],
      ['reconfiguring', true, 'KRAKEN_BUSY']]) {
      const previous = suiteState[field]; suiteState[field] = invalid;
      const drift = await request(apiPort, 'PUT', '/api/config?restart=false', matchedTarget, revision, intent);
      assert.equal(drift.status, 409); assert.equal(drift.body.code, code);
      assert.equal(commands.length, beforeMatchedCommands);
      suiteState[field] = previous;
    }

    // A file race after receiver confirmation is necessarily partial: the API
    // must not overwrite the external edit or pretend both sides committed.
    const partialCandidate = clone(changed);
    partialCandidate.capture.fc += 1000000;
    suiteBehavior = 'external-edit';
    const partial = await request(apiPort, 'PUT', '/api/config?restart=false',
      partialCandidate, revision, intent);
    assert.equal(partial.status, 409);
    assert.equal(partial.body.code, 'RECEIVER_SYNC_PERSISTENCE_CONFLICT');
    assert.equal(partial.body.receiverSync.status, 'partial');
    assert.equal(partial.body.receiverSync.configPersisted, false);
    assert.match(partial.body.errors.join(' '), /Suite V2 may already have applied/);
    let disk = yaml.load(fs.readFileSync(filename, 'utf8'));
    assert.equal(disk.capture.fc, changed.capture.fc);
    assert.equal(disk.location.rx.name, 'Concurrent external edit');
    const externallyEdited = await request(apiPort, 'GET', '/api/config');
    revision = externallyEdited.headers.etag.replace(/^"|"$/g, '');
    const persistenceReceipt = (await request(apiPort, 'GET', '/api/system/status')).body.receiverSynchronization;
    const pendingAfterPersistence = await request(apiPort, 'PUT', '/api/config?mode=pending&restart=false', externallyEdited.body, revision,
      {...intent, 'X-VectorWarp-Receiver-Sync': 'save-pending-v1'});
    assert.equal(pendingAfterPersistence.body.code, 'RECEIVER_RECONCILIATION_REQUIRED');
    assert.deepEqual((await request(apiPort, 'GET', '/api/system/status')).body.receiverSynchronization, persistenceReceipt);
    const reconcile = clone(externallyEdited.body);
    reconcile.capture.fc = partialCandidate.capture.fc;
    const commandsBeforeReconcile = commands.length;
    const reconciled = await request(apiPort, 'PUT', '/api/config?restart=false',
      reconcile, revision, intent);
    assert.equal(reconciled.status, 200, JSON.stringify(reconciled.body));
    assert.equal(reconciled.body.receiverSync.status, 'already-matched');
    assert.equal(commands.length, commandsBeforeReconcile,
      'Reconciliation must use Suite readback instead of repeating a command');
    revision = reconciled.body.revision;

    const missingIntent = clone(reconcile);
    missingIntent.capture.fc += 1000000;
    const commandsBeforeMissingIntent = commands.length;
    const rejectedIntent = await request(apiPort, 'PUT', '/api/config?restart=false',
      missingIntent, revision, {Origin: `http://127.0.0.1:${apiPort}`});
    assert.equal(rejectedIntent.status, 428);
    assert.equal(rejectedIntent.body.code, 'RECEIVER_SYNC_INTENT_REQUIRED');
    assert.equal(commands.length, commandsBeforeMissingIntent);
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.fc,
      reconcile.capture.fc);

    suiteBehavior = 'reject';
    const refused = await request(apiPort, 'PUT', '/api/config?restart=false',
      missingIntent, revision, intent);
    assert.equal(refused.status, 409);
    assert.equal(refused.body.code, 'KRAKEN_COMMAND_REJECTED');
    assert.equal(refused.body.receiverSync.status, 'failed');
    assert.equal(refused.body.receiverSync.configPersisted, false);
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.fc,
      reconcile.capture.fc, 'A rejected command must leave YAML unchanged');

    suiteBehavior = 'ack-only';
    const unconfirmed = await request(apiPort, 'PUT', '/api/config?restart=false',
      missingIntent, revision, intent);
    assert.equal(unconfirmed.status, 504);
    assert.equal(unconfirmed.body.code, 'KRAKEN_READBACK_TIMEOUT');
    assert.equal(unconfirmed.body.receiverSync.status, 'partial');
    assert.equal(unconfirmed.body.receiverSync.operations[0].acknowledged, true);
    assert.equal(unconfirmed.body.receiverSync.operations[0].readbackMatched, false);
    assert.equal(yaml.load(fs.readFileSync(filename, 'utf8')).capture.fc,
      reconcile.capture.fc, 'Acknowledgement without readback must leave YAML unchanged');

    suiteState.sampleRate = 2000000;
    const commandsBeforeMismatch = commands.length;
    const mismatch = await request(apiPort, 'PUT', '/api/config?restart=false',
      missingIntent, revision, intent);
    assert.equal(mismatch.status, 409);
    assert.equal(mismatch.body.code, 'KRAKEN_SAMPLE_RATE_MISMATCH');
    assert.equal(commands.length, commandsBeforeMismatch,
      'Sample-rate mismatch must be rejected before a command');
    suiteState.sampleRate = original.capture.fs;

    const stale = await request(apiPort, 'PUT', '/api/config?restart=false',
      missingIntent, 'stale-revision', intent);
    assert.equal(stale.status, 409);
    assert.equal(commands.length, commandsBeforeMismatch,
      'A stale config revision must be rejected before a command');

    const direct = clone(reconcile);
    direct.capture.fs = 2000000;
    direct.capture.device = {type: 'Usrp', address: 'type=b200',
      subdev: 'A:A A:B', antenna: ['RX2', 'RX2'], gain: [20, 20]};
    delete direct.process.reference_synthesis;
    const directResult = await request(apiPort, 'PUT', '/api/config?restart=false',
      direct, revision, intent);
    assert.equal(directResult.status, 200, JSON.stringify(directResult.body));
    assert.equal(directResult.body.receiverSync.priorOutcomeUnverified, true,
      'Switching backend does not pretend the previous upstream uncertainty was repaired');
    assert.ok(directResult.body.receiverSync.priorReceiverReceipt);
    assert.equal((await request(apiPort, 'GET', '/api/system/status')).body.receiverSynchronization.reconciliationRequired, false,
      'A deliberate direct-backend Apply must permit its own SDK startup checks');
    assert.equal(directResult.body.receiverSync.status, 'not-required');
    assert.match(directResult.body.receiverSync.acceptance.acknowledgement,
      /no positive applied-settings acknowledgement/);
    assert.equal(directResult.body.receiverSync.acceptance.hardwareReadback, false);
    assert.equal(commands.length, commandsBeforeMismatch,
      'A direct receiver profile must not be sent to Suite V2');

    const replay = clone(reconcile); replay.capture.replay.state = true;
    const savedReplay = await request(apiPort, 'PUT', '/api/config?restart=false', replay, directResult.body.revision, intent);
    assert.equal(savedReplay.status, 200, JSON.stringify(savedReplay.body));
    const live = clone(replay); live.capture.replay.state = false;
    suiteState.frequency = live.capture.fc - 1000000;
    const beforeLive = commands.length;
    const resumed = await request(apiPort, 'PUT', '/api/config?restart=false', live, savedReplay.body.revision, intent);
    assert.equal(resumed.status, 200, JSON.stringify(resumed.body));
    assert.deepEqual(commands.slice(beforeLive), [{command: 'set_frequency', frequency: live.capture.fc}],
      'Replay-to-live transition must synchronize even when stored tuning/count did not change');

    suiteState.sampleRate = 2000000;
    const deniedRestart = await request(apiPort, 'PUT', '/api/config?restart=true', resumed.body.config, resumed.body.revision, intent);
    assert.equal(deniedRestart.status, 409);
    assert.equal(deniedRestart.body.code, 'KRAKEN_SAMPLE_RATE_MISMATCH',
      'Save + Restart must require matching full-state readback before scheduling any restart');
    assert.equal((await request(apiPort, 'GET', '/api/system/status')).body.restart.state, 'idle');

    console.log('browser/API to simulated Suite V2 synchronization tests passed');
  } finally {
    if (child) child.kill('SIGTERM');
    for (const socket of sockets) socket.destroy();
    await new Promise(resolve => suite.close(() => resolve()));
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => {
  console.error(error.stack || error);
  if (childError) console.error(childError);
  process.exitCode = 1;
});
