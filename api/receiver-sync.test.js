'use strict';

const assert = require('assert');
const net = require('net');
const {createKrakenControlClient, createReceiverSynchronizer,
  receiverAcceptanceBoundary, requiresReceiverSynchronization} = require('./receiver-sync');

function config(port, overrides = {}) {
  const value = {
    capture: {
      fs: 2400000,
      fc: 204640000,
      replay: {state: false},
      device: {
        type: 'Kraken', channel_count: 5,
        heimdall: {host: '127.0.0.1', port: port + 1, control_port: port},
        reference_channel: 0, surveillance_channels: [1, 2, 3, 4]
      }
    },
    process: {reference_synthesis: {mode: 'dedicated', channels: [0]}}
  };
  Object.assign(value.capture, overrides.capture || {});
  if (overrides.device) Object.assign(value.capture.device, overrides.device);
  return value;
}

function status(state) {
  return {settings: {center_freq: state.frequency, sample_rate: state.sampleRate,
    gain: 20}, num_channels: state.channels, max_elements: state.maximum,
  reconfiguring: state.reconfiguring, recovering: state.recovering || false,
  operating_mode: state.mode || 'coherent', cooldown_active: false};
}

async function simulatedSuite(initial, onCommand) {
  const commands = [];
  const sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket);
    socket.setEncoding('utf8');
    socket.write(`${JSON.stringify(status(initial))}\n`);
    let buffer = '';
    socket.on('data', chunk => {
      buffer += chunk;
      const lines = buffer.split('\n');
      buffer = lines.pop();
      for (const line of lines) {
        if (!line) continue;
        const command = JSON.parse(line);
        commands.push(command);
        onCommand(command, socket, initial);
      }
    });
    socket.on('close', () => sockets.delete(socket));
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  return {
    port: server.address().port,
    commands,
    async close() {
      for (const socket of sockets) socket.destroy();
      await new Promise(resolve => server.close(resolve));
    }
  };
}

async function oneShotSuite(onConnect) {
  const sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    onConnect(socket);
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  return {port: server.address().port, async close() {
    for (const socket of sockets) socket.destroy();
    await new Promise(resolve => server.close(resolve));
  }};
}

async function rejected(promise, code, pattern) {
  try { await promise; }
  catch (error) {
    assert.equal(error.code, code);
    assert.match(error.message, pattern);
    return error;
  }
  assert.fail(`Expected ${code}`);
}

(async () => {
  const state = {frequency: 100000000, sampleRate: 2400000,
    channels: 5, maximum: 8, reconfiguring: false};
  const suite = await simulatedSuite(state, (command, socket, live) => {
    if (command.command === 'set_frequency') {
      // An unsolicited old status may race the command response. It must not
      // be mistaken for acknowledgement or readback.
      socket.write(`${JSON.stringify(status(live))}\n`);
      socket.write(`${JSON.stringify({status: 'success', frequency: command.frequency})}\n`);
      live.frequency = command.frequency;
      socket.write(`${JSON.stringify(status(live))}\n`);
    } else if (command.command === 'set_num_elements') {
      socket.write(`${JSON.stringify({status: 'success',
        num_elements: command.num_elements,
        message: 'reconfiguration started (devices reopen + full recalibration)'})}\n`);
      live.reconfiguring = true;
      socket.write(`${JSON.stringify(status(live))}\n`);
      setTimeout(() => {
        live.channels = command.num_elements;
        live.reconfiguring = false;
        socket.write(`${JSON.stringify(status(live))}\n`);
      }, 10);
    }
  });
  try {
    const candidate = config(suite.port, {capture: {fc: 204640000},
      device: {channel_count: 3, surveillance_channels: [1, 2]}});
    const result = await createKrakenControlClient({statusTimeoutMs: 300,
      readbackTimeoutMs: 300}).synchronize(candidate);
    assert.equal(result.status, 'synchronized');
    assert.deepEqual(suite.commands, [
      {command: 'set_num_elements', num_elements: 3},
      {command: 'set_frequency', frequency: 204640000}
    ]);
    assert.deepEqual(result.operations.map(item => [item.operation,
      item.acknowledged, item.readbackMatched]), [
      ['set_num_elements', true, true], ['set_frequency', true, true]
    ]);
    assert.equal(result.after.centerFrequency, 204640000);
    assert.equal(result.after.channelCount, 3);
    assert.equal(result.channelIdentity.hardwareSerialsVerified, false);
    assert.equal(result.hardwareVerified, false);
  } finally { await suite.close(); }

  const refusalState = {frequency: 100000000, sampleRate: 2400000,
    channels: 5, maximum: 5, reconfiguring: false};
  const refusal = await simulatedSuite(refusalState, (command, socket) => {
    socket.write(`${JSON.stringify({status: 'error', message: 'tuner rejected request'})}\n`);
  });
  try {
    const error = await rejected(createKrakenControlClient({statusTimeoutMs: 200,
      readbackTimeoutMs: 200}).synchronize(config(refusal.port, {
      capture: {fc: 101000000}})), 'KRAKEN_COMMAND_REJECTED', /tuner rejected/);
    assert.equal(error.receiverSync.status, 'failed');
    assert.equal(error.receiverSync.operations[0].acknowledged, false);
  } finally { await refusal.close(); }

  const wrongAckState = {frequency: 100000000, sampleRate: 2400000,
    channels: 5, maximum: 5, reconfiguring: false};
  const wrongAck = await simulatedSuite(wrongAckState, (_command, socket) => {
    socket.write(`${JSON.stringify({status: 'success', frequency: 99999999})}\n`);
  });
  try {
    const error = await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 100}).synchronize(config(wrongAck.port,
      {capture: {fc: 101000000}})), 'KRAKEN_ACKNOWLEDGEMENT_MISMATCH',
    /unexpected value/);
    assert.equal(error.receiverSync.status, 'indeterminate');
    assert.equal(error.receiverSync.operations[0].commandOutcome, 'unknown');
  } finally { await wrongAck.close(); }

  const noReadbackState = {frequency: 100000000, sampleRate: 2400000,
    channels: 5, maximum: 5, reconfiguring: false};
  const noReadback = await simulatedSuite(noReadbackState, (command, socket) => {
    socket.write(`${JSON.stringify({status: 'success', frequency: command.frequency})}\n`);
  });
  try {
    const error = await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 60}).synchronize(config(noReadback.port, {
      capture: {fc: 102000000}})), 'KRAKEN_READBACK_TIMEOUT', /did not acknowledge and report/);
    assert.equal(error.receiverSync.status, 'partial');
    assert.equal(error.receiverSync.operations[0].acknowledged, true);
    assert.equal(error.receiverSync.operations[0].readbackMatched, false);
  } finally { await noReadback.close(); }

  const staleAcrossState = {frequency: 100000000, sampleRate: 2400000,
    channels: 5, maximum: 8, reconfiguring: false};
  const staleAcrossOperations = await simulatedSuite(staleAcrossState,
    (command, socket, live) => {
      if (command.command === 'set_num_elements') {
        socket.write(`${JSON.stringify({status: 'success',
          num_elements: command.num_elements})}\n`);
        live.channels = command.num_elements;
        // Frequency changed before its command. The count stage must reject
        // this unexpected intermediate tuple without issuing another command.
        live.frequency = 102000000;
        socket.write(`${JSON.stringify(status(live))}\n`);
      } else {
        socket.write(`${JSON.stringify({status: 'success',
          frequency: command.frequency})}\n`);
      }
    });
  try {
    const error = await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 60}).synchronize(config(staleAcrossOperations.port, {
      capture: {fc: 102000000}, device: {channel_count: 3}})),
    'KRAKEN_READBACK_TIMEOUT', /did not acknowledge and report/);
    assert.deepEqual(error.receiverSync.operations.map(item =>
      [item.operation, item.readbackMatched]), [
      ['set_num_elements', false]
    ]);
  } finally { await staleAcrossOperations.close(); }

  const sampleState = {frequency: 204640000, sampleRate: 1024000,
    channels: 5, maximum: 5, reconfiguring: false};
  const sampleMismatch = await simulatedSuite(sampleState, () => {
    assert.fail('A sample-rate mismatch must be rejected before any command is sent.');
  });
  try {
    await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 100}).synchronize(config(sampleMismatch.port)),
    'KRAKEN_SAMPLE_RATE_MISMATCH', /startup\/build setting/);
    assert.deepEqual(sampleMismatch.commands, []);
  } finally { await sampleMismatch.close(); }

  const widebandState = {frequency: 204640000, sampleRate: 2400000,
    channels: 5, maximum: 8, reconfiguring: false, mode: 'wideband'};
  const wideband = await simulatedSuite(widebandState, () => {
    assert.fail('Wideband mode must be rejected before sending a command.');
  });
  try {
    await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 100}).synchronize(config(wideband.port,
      {capture: {fc: 205000000}})), 'KRAKEN_MODE_MISMATCH', /wideband mode/);
    assert.deepEqual(wideband.commands, []);
  } finally { await wideband.close(); }

  const rangeState = {frequency: 204640000, sampleRate: 2400000,
    channels: 5, maximum: 8, reconfiguring: false};
  const outOfRange = await simulatedSuite(rangeState, () => {
    assert.fail('Unadvertised wideband range must not be inferred.');
  });
  try {
    await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 100}).synchronize(config(outOfRange.port,
      {capture: {fc: 2000000000}})), 'KRAKEN_FREQUENCY_NOT_AUTOMATABLE',
    /24–1766 MHz/);
    assert.deepEqual(outOfRange.commands, []);
  } finally { await outOfRange.close(); }

  const countState = {frequency: 204640000, sampleRate: 2400000,
    channels: 5, maximum: 5, reconfiguring: false};
  const tooMany = await simulatedSuite(countState, () => {
    assert.fail('An unavailable channel count must be rejected before sending.');
  });
  try {
    await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 100}).synchronize(config(tooMany.port,
      {device: {channel_count: 8}})), 'KRAKEN_CHANNEL_COUNT_UNAVAILABLE',
    /5 configured channel identities/);
    assert.deepEqual(tooMany.commands, []);
  } finally { await tooMany.close(); }

  const countMatrixState = {frequency: 204640000, sampleRate: 2400000,
    channels: 5, maximum: 8, reconfiguring: false};
  const countMatrix = await simulatedSuite(countMatrixState,
    (command, socket, live) => {
      assert.equal(command.command, 'set_num_elements');
      socket.write(`${JSON.stringify({status: 'success',
        num_elements: command.num_elements})}\n`);
      live.reconfiguring = true;
      socket.write(`${JSON.stringify(status(live))}\n`);
      setTimeout(() => {
        live.channels = command.num_elements;
        live.reconfiguring = false;
        socket.write(`${JSON.stringify(status(live))}\n`);
      }, 2);
    });
  try {
    for (let channelCount = 2; channelCount <= 8; channelCount += 1) {
      const result = await createKrakenControlClient({statusTimeoutMs: 100,
        readbackTimeoutMs: 100}).synchronize(config(countMatrix.port,
        {device: {channel_count: channelCount}}));
      assert.equal(result.after.channelCount, channelCount,
        `${channelCount}-channel simulated readback`);
      assert.equal(result.after.reconfiguring, false,
        `${channelCount}-channel reconfiguration must finish`);
    }
  } finally { await countMatrix.close(); }

  const malformed = await oneShotSuite(socket => socket.end('{not-json}\n'));
  try {
    await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 100}).synchronize(config(malformed.port)),
    'KRAKEN_PROTOCOL_MISMATCH', /malformed JSON/);
  } finally { await malformed.close(); }

  const invalidStatus = await oneShotSuite(socket => socket.end(
    `${JSON.stringify({settings: {center_freq: 204640000}})}\n`));
  try {
    await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 100}).synchronize(config(invalidStatus.port)),
    'KRAKEN_PROTOCOL_MISMATCH', /missing required/);
  } finally { await invalidStatus.close(); }

  const closed = await oneShotSuite(socket => socket.end());
  try {
    await rejected(createKrakenControlClient({statusTimeoutMs: 100,
      readbackTimeoutMs: 100}).synchronize(config(closed.port)),
    'KRAKEN_CONNECTION_CLOSED', /closed.*before/i);
  } finally { await closed.close(); }

  const before = config(8092);
  const same = JSON.parse(JSON.stringify(before));
  same.location = {rx: {name: 'changed only locally'}};
  assert.equal(requiresReceiverSynchronization(before, same), false);
  same.capture.fc += 1;
  assert.equal(requiresReceiverSynchronization(before, same), true);
  same.capture.replay.state = true;
  assert.equal(requiresReceiverSynchronization(before, same), false);

  const direct = {capture: {replay: {state: false}, device: {type: 'Usrp'}}};
  assert.deepEqual(receiverAcceptanceBoundary(direct), {
    receiverType: 'Usrp', mode: 'live',
    configurationAuthority: 'VectorWarp startup configuration',
    writePath: 'processor startup through UHD multi_usrp',
    acknowledgement: 'UHD exceptions can report failure; no positive applied-settings acknowledgement is emitted',
    positiveAppliedAcknowledgement: false,
    readbackBoundary: 'UHD startup getters gate frequency, rate, gain, antenna and subdevice mapping before streaming; processor status does not carry an applied-values receipt.',
    hardwareReadback: false, physicalReceiverVerified: false
  });
  const synchronizer = createReceiverSynchronizer({kraken: {
    synchronize: async () => assert.fail('Direct receivers have no external synchronization call.')
  }});
  assert.equal((await synchronizer.synchronize(direct, direct)).status, 'not-required');

  console.log('receiver synchronization protocol tests passed (simulated Suite only)');
})().catch(error => {
  console.error(error.stack || error);
  process.exitCode = 1;
});
