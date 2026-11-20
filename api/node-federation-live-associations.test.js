'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const express = require('express');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const {installNodeFederation, mergeLiveAircraftAssociations} = require('./node-federation');

function listen(server) {
  return new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
}

function close(server) {
  return new Promise(resolve => server.close(resolve));
}

function getJson(url) {
  return new Promise((resolve, reject) => http.get(url, response => {
    const chunks = [];
    response.on('data', chunk => chunks.push(chunk));
    response.on('end', () => {
      try { resolve({status: response.statusCode, value: JSON.parse(Buffer.concat(chunks))}); }
      catch (error) { reject(error); }
    });
  }).on('error', reject));
}

test('merges live association evidence by aircraft and preserves channel/source labels', () => {
  const associations = mergeLiveAircraftAssociations([
    {entry: {label: 'Local'}, value: {associations: [{hex: 'AbC123', flight: 'TEST1',
      detected: true, channels: ['primary'], detected_cpis: 3, last_at: 10}]}},
    {entry: {label: 'Remote'}, value: {associations: [{hex: 'abc123', tracked: true,
      channels: ['secondary'], sources: ['relay'], detected_cpis: 2, tracked_cpis: 3, last_at: 20}]}}
  ]);
  assert.deepEqual(associations, [{hex: 'abc123', flight: 'TEST1', detected: true, tracked: true,
    channels: ['primary', 'secondary'], sources: ['Local', 'Remote', 'relay'],
    detected_cpis: 5, tracked_cpis: 3, last_at: 20}]);
  assert.equal(mergeLiveAircraftAssociations([{value: {associations: [
    {hex: 'one', detected: true}, {hex: 'two', detected: true}]}}], 1).length, 1,
  'the aggregate has a fixed aircraft bound');
});

test('federated live-association endpoint joins registered nodes and remains fail-soft', async t => {
  const remote = http.createServer((request, response) => {
    if (request.url !== '/api/live-aircraft-associations') return response.writeHead(404).end();
    response.writeHead(200, {'Content-Type': 'application/json', 'X-VectorWarp-Node-Id': 'remote'});
    response.end(JSON.stringify({associations: [{hex: 'abc123', tracked: true,
      channels: ['secondary'], sources: ['relay'], tracked_cpis: 3, last_at: 20}]}));
  });
  await listen(remote);
  t.after(() => close(remote));
  const registryDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-federation-'));
  const registryFile = path.join(registryDirectory, 'nodes.json');
  const remotePort = remote.address().port;
  fs.writeFileSync(registryFile, JSON.stringify({nodes: [
    {id: 'remote', label: 'Remote', expectedIdentity: 'remote', url: `http://127.0.0.1:${remotePort}`},
    {id: 'unavailable', label: 'Unavailable', expectedIdentity: 'unavailable', url: 'http://127.0.0.1:9'}
  ]}));
  fs.chmodSync(registryFile, 0o600);
  t.after(() => fs.rmSync(registryDirectory, {recursive: true, force: true}));
  const app = express();
  const coordinator = http.createServer(app);
  await listen(coordinator);
  t.after(() => close(coordinator));
  const port = coordinator.address().port;
  installNodeFederation(app, registryFile, {config: {network: {ports: {api: port}}}});
  app.get('/api/live-aircraft-associations', (_request, response) => response.json({associations: [
    {hex: 'ABC123', flight: 'TEST1', detected: true, channels: ['primary'], detected_cpis: 3, last_at: 10}
  ]}));
  const result = await getJson(`http://127.0.0.1:${port}/api/federation/live-aircraft-associations`);
  assert.equal(result.status, 200);
  assert.equal(result.value.schema, 'vectorwarp-federated-live-aircraft-associations-v1');
  assert.deepEqual(result.value.associations, [{hex: 'abc123', flight: 'TEST1', detected: true,
    tracked: true, channels: ['primary', 'secondary'], sources: ['Local', 'Remote', 'relay'],
    detected_cpis: 3, tracked_cpis: 3, last_at: 20}]);
});
