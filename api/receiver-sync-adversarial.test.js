'use strict';
const assert = require('assert');
const net = require('net');
const {createKrakenControlClient, requiresReceiverSynchronization} = require('./receiver-sync');
const clone = value => JSON.parse(JSON.stringify(value));
const config = () => ({capture: {fc: 102000000, fs: 2400000, replay: {state: false}, device: {
  type: 'Kraken', channel_count: 5, heimdall: {host: '127.0.0.1', port: 8091, control_port: 8092},
  reference_channel: 0, surveillance_channels: [1, 2, 3, 4]}}, process: {reference_synthesis: {mode: 'dedicated', channels: [0]}}});
const status = (patch = {}) => ({settings: {center_freq: 100000000, sample_rate: 2400000},
  num_channels: 5, max_elements: 8, reconfiguring: false, recovering: false,
  operating_mode: 'coherent', cooldown_active: false, ...patch});
async function exercise(handler, candidate = config()) {
  const sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket); socket.on('close', () => sockets.delete(socket));
    const send = frame => { if (!socket.destroyed) socket.write(JSON.stringify(frame) + '\n'); };
    send(status()); let buffer = '';
    socket.on('data', chunk => { buffer += chunk; const lines = buffer.split('\n'); buffer = lines.pop();
      lines.filter(Boolean).forEach(line => handler(JSON.parse(line), send)); });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  candidate.capture.device.heimdall.control_port = server.address().port;
  try { return await createKrakenControlClient({statusTimeoutMs: 150, readbackTimeoutMs: 70}).synchronize(candidate); }
  finally { sockets.forEach(socket => socket.destroy()); await new Promise(resolve => server.close(resolve)); }
}
const cases = [];
const test = (name, run) => cases.push({name, run});
test('status before ACK cannot complete the operation', async () => {
  await assert.rejects(exercise((command, send) => {
    send(status({settings: {center_freq: command.frequency, sample_rate: 2400000}}));
    send({status: 'success', frequency: command.frequency});
  }));
});
for (const [label, patch] of [['sample-rate drift', {settings: {center_freq: 102000000, sample_rate: 2000000}}],
  ['mode drift', {operating_mode: 'wideband'}], ['channel-count drift', {num_channels: 3}],
  ['reconfiguration still active', {reconfiguring: true}]]) {
  test(`frequency readback rejects ${label}`, async () => {
    await assert.rejects(exercise((command, send) => {
      send({status: 'success', frequency: command.frequency});
      send(status({settings: {center_freq: command.frequency, sample_rate: 2400000}, ...patch}));
    }));
  });
}
test('completed count must remain matched in final frequency readback', async () => {
  const candidate = config(); candidate.capture.device.channel_count = 3;
  await assert.rejects(exercise((command, send) => {
    if (command.command === 'set_num_elements') {
      send({status: 'success', num_elements: 3}); send(status({num_channels: 3}));
    } else {
      send({status: 'success', frequency: command.frequency});
      send(status({settings: {center_freq: command.frequency, sample_rate: 2400000}, num_channels: 5}));
    }
  }, candidate));
});
test('replay to live requires receiver synchronization even with unchanged settings', () => {
  const candidate = config(), previous = clone(candidate); previous.capture.replay.state = true;
  assert.equal(requiresReceiverSynchronization(previous, candidate), true);
});
test('new IQ source port requires a fresh control observation', () => {
  const candidate = config(), previous = clone(candidate); candidate.capture.device.heimdall.port++;
  assert.equal(requiresReceiverSynchronization(previous, candidate), true);
});
test('ACK followed by a matching coherent status succeeds without verification claims', async () => {
  const result = await exercise((command, send) => {
    send({status: 'success', frequency: command.frequency});
    send(status({settings: {center_freq: command.frequency, sample_rate: 2400000}}));
  });
  assert.equal(result.status, 'synchronized'); assert.equal(result.hardwareVerified, false);
  assert.equal(result.calibrationVerified, false); assert.equal(result.channelIdentity.hardwareSerialsVerified, false);
});
(async () => {
  let failures = 0;
  for (const item of cases) {
    try { await item.run(); console.log(`PASS ${item.name}`); }
    catch (error) { failures++; console.log(`FAIL ${item.name}: ${error.message}`); }
  }
  console.log(`${cases.length - failures}/${cases.length} adversarial receiver transaction gates passed.`);
  process.exitCode = failures ? 1 : 0;
})().catch(error => { console.error(error); process.exitCode = 1; });
