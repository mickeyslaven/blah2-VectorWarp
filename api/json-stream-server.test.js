'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
let stderr = '';

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

function request(port, route) {
  return new Promise((resolve, reject) => {
    const req = http.get({host: '127.0.0.1', port, path: route}, response => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', chunk => { body += chunk; });
      response.on('end', () => resolve({status: response.statusCode, body}));
    });
    req.on('error', reject);
  });
}

async function waitFor(port, route, expected) {
  for (let attempt = 0; attempt < 80; ++attempt) {
    try {
      const response = await request(port, route);
      if (expected(response)) return response;
    } catch (_) { /* The child may still be binding. */ }
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  throw new Error(`Timed out waiting for ${route}`);
}

function connect(port) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({host: '127.0.0.1', port}, () => resolve(socket));
    socket.once('error', reject);
  });
}

function close(socket) {
  return new Promise(resolve => {
    if (socket.destroyed) return resolve();
    socket.once('close', resolve);
    socket.end();
  });
}

async function send(port, chunks) {
  const socket = await connect(port);
  for (const chunk of chunks) socket.write(chunk);
  await close(socket);
}

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-json-stream-'));
  const configPath = path.join(directory, 'config.yml');
  let child;
  try {
    const source = path.join(__dirname, '..', 'config', 'config-kraken.yml');
    const config = yaml.load(fs.readFileSync(source, 'utf8'));
    const ports = await reservePorts(Object.keys(config.network.ports).length);
    Object.keys(config.network.ports).forEach((name, index) => { config.network.ports[name] = ports[index]; });
    config.network.ip = '127.0.0.1';
    fs.writeFileSync(configPath, yaml.dump(config));
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), configPath], {
      env: {...process.env, BLAH2_RECEIVER_TYPES: 'Kraken'}, stdio: ['ignore', 'ignore', 'pipe']
    });
    child.stderr.on('data', chunk => { stderr += chunk; });
    const apiPort = config.network.ports.api;
    await waitFor(apiPort, '/api/system/status', response => response.status === 200);

    const routes = [
      ['map', '/api/map'], ['detection', '/api/detection'], ['track', '/api/tracker'],
      ['timing', '/api/timing'], ['iqdata', '/api/iqdata']
    ];
    for (const [name, route] of routes) {
      const first = JSON.stringify({stream: name, message: `fragmented 🙂 ${name}`});
      const second = JSON.stringify({stream: name, sequence: 2});
      const bytes = Buffer.from(first);
      // Split the emoji across byte chunks and coalesce the final first frame
      // with a complete second frame on the same TCP connection.
      const emoji = bytes.indexOf(Buffer.from('🙂'));
      const chunks = [bytes.subarray(0, emoji + 2),
        Buffer.concat([bytes.subarray(emoji + 2), Buffer.from(second)])];
      // Timing deliberately expires on disconnect, unlike the historical
      // display streams, so observe its complete frame before closing it.
      const liveSocket = name === 'timing' ? await connect(config.network.ports[name]) : null;
      if (liveSocket) chunks.forEach(chunk => liveSocket.write(chunk));
      else await send(config.network.ports[name], chunks);
      const response = await waitFor(apiPort, route, value => value.body === second);
      assert.equal(response.body, second, `${name} must retain the final coalesced frame`);
      if (liveSocket) await close(liveSocket);
    }

    // A partial frame belongs only to its original socket and cannot contaminate
    // the next processor connection.
    await send(config.network.ports.map, [Buffer.from('{"stale":')]);
    const recovered = JSON.stringify({stream: 'map', reconnect: true});
    await send(config.network.ports.map, [Buffer.from(recovered)]);
    assert.equal((await waitFor(apiPort, '/api/map', value => value.body === recovered)).body, recovered);

    const broken = await connect(config.network.ports.map);
    broken.on('error', () => {});
    broken.write('{"broken":]');
    await new Promise(resolve => broken.once('close', resolve));
    const unhealthy = JSON.parse((await waitFor(apiPort, '/api/system/status', response => {
      try { return JSON.parse(response.body).errors.some(error => error.includes('map connection')); }
      catch (_) { return false; }
    })).body);
    assert.ok(unhealthy.errors.some(error => error.includes('map connection')));

    const oversized = await connect(config.network.ports.map);
    oversized.on('error', () => {});
    oversized.write(Buffer.concat([Buffer.from('{"payload":"'),
      Buffer.alloc(16 * 1024 * 1024, 'x')]));
    await new Promise(resolve => oversized.once('close', resolve));
    const bounded = JSON.parse((await waitFor(apiPort, '/api/system/status', response => {
      try { return JSON.parse(response.body).errors.some(error => error.includes('exceeds limit')); }
      catch (_) { return false; }
    })).body);
    assert.ok(bounded.errors.some(error => error.includes('exceeds limit')),
      'Oversized input must terminate only its own TCP connection');

    const healthy = JSON.stringify({stream: 'map', recoveredAfterError: true});
    await send(config.network.ports.map, [Buffer.from(healthy)]);
    const status = JSON.parse((await waitFor(apiPort, '/api/system/status', response => {
      try { return !JSON.parse(response.body).errors.some(error => error.includes('map connection')); }
      catch (_) { return false; }
    })).body);
    assert.ok(!status.errors.some(error => error.includes('map connection')),
      'A valid reconnect must clear only its prior connection health error');
    assert.equal((await request(apiPort, '/api/map')).body, healthy);
    assert.equal(child.exitCode, null, 'Malformed input must not stop the API process');
    console.log('JSON stream server fragmentation, isolation, error containment and recovery tests passed.');
  } finally {
    if (child && child.exitCode === null) child.kill('SIGTERM');
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => {
  console.error(error);
  if (stderr) console.error(stderr);
  process.exitCode = 1;
});
