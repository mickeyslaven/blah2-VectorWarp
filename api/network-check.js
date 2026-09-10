'use strict';

const net = require('net');

function bindMessage(field, code) {
  if (code === 'EADDRINUSE') return `${field} is already in use. Choose a free port.`;
  if (code === 'EADDRNOTAVAIL') return `${field}: this IP address is not assigned to the API host.`;
  if (code === 'EACCES') return `${field}: the API does not have permission to use this port.`;
  return `${field}: unable to open this address/port (${code || 'unknown error'}).`;
}

function probeBind(host, port) {
  return new Promise(resolve => {
    const server = net.createServer();
    let timer;
    let settled = false;
    const finish = code => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (server.listening) server.close(() => resolve(code));
      else resolve(code);
    };
    server.on('error', error => finish(error.code));
    timer = setTimeout(() => finish('timeout'), 1000);
    try {
      server.listen({host, port, exclusive: true}, () => {
        if (settled) server.close();
        else finish(null);
      });
    } catch (error) { finish(error.code); }
  });
}

async function checkNetworkBindings(candidate, running, ownedPorts, probe = probeBind) {
  const next = candidate.network;
  const checks = [];
  if (next.ip !== running.network.ip) {
    const error = await probe(next.ip, 0);
    if (error) return [bindMessage('network.ip', error)];
  }
  // Existing listeners are released by the restart. Check newly selected ports
  // without disconnecting radar. A bind check cannot reserve a port against a
  // different process taking it between validation and restart.
  for (const name of ['api', 'map', 'detection', 'track', 'timestamp', 'timing', 'iqdata']) {
    const port = next.ports[name];
    if (!ownedPorts.has(port)) checks.push(probe(next.ip, port).then(code =>
      code ? bindMessage(`network.ports.${name}`, code) : null));
  }
  return (await Promise.all(checks)).filter(Boolean);
}

module.exports = {bindMessage, checkNetworkBindings, probeBind};
