'use strict';
const assert = require('assert');
const net = require('net');
const {checkNetworkBindings, probeBind, bindMessage} = require('./network-check');
(async () => {
  const listener = net.createServer();
  await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
  try {
    const port = listener.address().port;
    assert.equal(await probeBind('127.0.0.1', port), 'EADDRINUSE');
    assert.equal(await probeBind('127.0.0.1', 0), null);
    const ports = Object.fromEntries(['api', 'map', 'detection', 'track', 'timestamp', 'timing', 'iqdata'].map((name, i) => [name, 20000 + i]));
    const running = {network: {ip: '127.0.0.1', ports}};
    const owned = new Set(Object.values(ports));
    assert.deepEqual(await checkNetworkBindings(running, running, owned, () => {throw Error('Do not probe owned listeners');}), []);
    const candidate = {network: {ip: '127.0.0.1', ports: {...ports, map: port}}};
    assert.deepEqual(await checkNetworkBindings(candidate, running, owned),
      ['network.ports.map is already in use. Choose a free port.']);
    const moved = {network: {ip: '192.0.2.1', ports}};
    assert.deepEqual(await checkNetworkBindings(moved, running, owned, async () => 'EADDRNOTAVAIL'),
      ['network.ip: this IP address is not assigned to the API host.']);
    assert.ok(bindMessage('Port', 'EACCES').includes('permission'));
    console.log('Network bind preflight tests passed: occupied ports, owned listeners and unavailable addresses.');
  } finally { listener.close(); }
})().catch(error => {console.error(error); process.exitCode = 1;});
