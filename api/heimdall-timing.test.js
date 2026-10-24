'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {verifyBundledLocalOwner} = require('./receiver-start-check');
const {timingBytes, validateTiming} = require('./heimdall-timing');

test('local processor cannot adopt an unrelated or failed Heimdall service', () => {
  const config = {capture: {device: {heimdall: {host: '127.0.0.1', port: 5000, control_port: 8092}}}};
  const ready = {pid: 42, status: 'ready', bindHost: '127.0.0.1', dataPort: 5000, controlPort: 8092};
  const options = {read: () => JSON.stringify(ready), execute: (_exe, args) => args[0] === 'show' ? '42' : ''};
  verifyBundledLocalOwner(config, options);
  ready.pid = 43;
  assert.throws(() => verifyBundledLocalOwner(config, options), /not owned/);
  assert.throws(() => verifyBundledLocalOwner(config, {...options, execute: () => {throw Error('inactive');}}), /inactive/);
  verifyBundledLocalOwner({capture: {device: {heimdall: {host: '192.0.2.10'}}}},
    {read: () => assert.fail(), execute: () => assert.fail()});
});

test('timed readers reject unknown versions and invalid anchor ranges', () => {
  assert.equal(timingBytes(0x4d434851), 0);
  const packet = Buffer.alloc(32 + 8 + 96);
  packet.writeUInt32BE(0x4d434854, 0); packet.writeUInt32BE(8, 8);
  packet.writeUInt32BE(1, 40); packet.writeUInt32BE(96, 44);
  packet.writeBigUInt64BE(8n, 128);
  validateTiming(packet, 1);
  packet.writeUInt32BE(2, 40);
  assert.throws(() => validateTiming(packet, 1), /Unsupported/);
  packet.writeUInt32BE(1, 40); packet.writeBigUInt64BE(7n, 128);
  assert.throws(() => validateTiming(packet, 1), /Unsupported/);
});
