'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('path');
const fs = require('fs');
const os = require('os');
const http = require('http');
const express = require('express');
const {configuredApiPort, defaultRegistryPath, nodeCapabilities, nodeRole, registry,
  installNodeFederation, localNodeOrigin, discoverLocalNode,
  nodeStatisticsTimeout} = require('./node-federation');

test('coordinator role requires an explicit portable declaration', () => {
  assert.equal(nodeRole({}), 'radar');
  assert.equal(nodeRole({VECTORWARP_NODE_ROLE: 'coordinator'}), 'coordinator');
  assert.throws(() => nodeRole({VECTORWARP_NODE_ROLE: 'typo'}), /Invalid/);
});

test('node capabilities distinguish offline ATSC diagnostics from live raw selection', () => {
  const processing = nodeCapabilities().processing;
  assert.deepEqual(processing.atsc_reference_modes, ['automatic', 'raw']);
  assert.deepEqual(processing.atsc_replay_reference_modes, ['automatic', 'raw', 'pilot_aided']);
  assert.equal(processing.reference_reconstruction.atsc_8vsb.live_selectable, false);
  assert.equal(processing.reference_reconstruction.atsc_8vsb.status, 'experimental-offline');
  assert.equal(processing.reference_reconstruction.atsc3_ofdm.live_selectable, false);
});

test('federation uses the same configured API port as the server', () => {
  assert.equal(configuredApiPort({network: {ports: {api: 43821}}}, {}), 43821);
  assert.equal(configuredApiPort({network: {ports: {api: 43821}}},
    {BLAH2_SETUP_PORT: '45123'}), 45123);
  assert.equal(configuredApiPort({}, {}), 3000);
});

test('statistics refresh survives bounded coordinator pressure without changing health probes', () => {
  assert.equal(nodeStatisticsTimeout({local: true}), 6000);
  assert.equal(nodeStatisticsTimeout({local: false}), 6000);
});

test('registry defaults are writable per-user paths on desktop platforms', () => {
  const home = path.join(path.sep, 'home', 'operator');
  assert.equal(defaultRegistryPath({}, 'darwin', home, 501),
    path.join(home, 'Library', 'Application Support', 'VectorWarp', 'nodes.json'));
  assert.equal(defaultRegistryPath({APPDATA: 'C:\\Users\\operator\\AppData\\Roaming'},
    'win32', 'C:\\Users\\operator', null),
  path.join('C:\\Users\\operator\\AppData\\Roaming', 'VectorWarp', 'nodes.json'));
  assert.equal(defaultRegistryPath({XDG_CONFIG_HOME: path.join(home, '.local-config')},
    'linux', home, 1000), path.join(home, '.local-config', 'vectorwarp', 'nodes.json'));
  assert.equal(defaultRegistryPath({}, 'linux', home, 0), '/etc/vectorwarp/nodes.json');
});

test('explicit registry path wins on every platform', () => {
  assert.equal(defaultRegistryPath({VECTORWARP_NODE_REGISTRY: '/srv/radar/nodes.json'},
    'darwin', '/Users/operator', 501), '/srv/radar/nodes.json');
});

// Loopback peers exercise registry and transport behavior without a receiver,
// service manager, site geometry, or an operator's network configuration.
async function serve(t, handler) {
  const server = http.createServer(handler);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => {
    server.close(resolve); server.closeAllConnections();
  }));
  return {server, port: server.address().port, origin: `http://127.0.0.1:${server.address().port}`};
}

function registryFile(t, nodes) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-portable-nodes-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  const filename = path.join(directory, 'nodes.json');
  if (nodes) fs.writeFileSync(filename, JSON.stringify({nodes}), {mode: 0o600});
  return filename;
}

async function request(origin, pathname, method = 'GET', body) {
  const response = await fetch(origin + pathname, {method, headers: {
    'Content-Type': 'application/json', 'X-VectorWarp-Intent':
      method === 'DELETE' ? 'local-node-remove-v1' : 'local-node-add-v1'},
  ...(body ? {body: JSON.stringify(body)} : {})});
  return {status: response.status, value: await response.json()};
}

async function peer(t, id = 'research-peer') {
  const state = {identity: id, capabilities: {...nodeCapabilities(), node_role: 'radar'},
    failProducts: false, held: null};
  const listener = await serve(t, async (req, res) => {
    const pathname = new URL(req.url, 'http://localhost').pathname;
    res.setHeader('X-VectorWarp-Node-Id', state.identity);
    res.setHeader('Content-Type', 'application/json');
    if (pathname === '/api/federation/identity') return res.end(JSON.stringify({identity: state.identity}));
    if (pathname === '/api/federation/capabilities') return res.end(JSON.stringify(state.capabilities));
    if (state.failProducts && !req.url.includes('summary=1')) return res.writeHead(503).end('{}');
    if (pathname === '/api/system/status') return res.end(JSON.stringify({radar: 'receiving',
      node_role: state.capabilities.node_role, processorFresh: true, processor: {state: 'live'}}));
    if (pathname === '/api/live-aircraft-associations') {
      if (state.held) await state.held;
      return res.end(JSON.stringify({associations: [{hex: 'abc123', tracked: true,
        channels: ['primary'], tracked_cpis: 2}]}));
    }
    if (pathname === '/api/live-scorecard/association-basis') return res.end(JSON.stringify({
      current_setup_lifetime: {eligible_passes: 4, tracked_passes: 2}}));
    if (pathname === '/api/association/passes') return res.end('{"passes":[]}');
    if (pathname === '/api/timing') return res.end('{"cpi":100}');
    res.end('{}');
  });
  return {...listener, state};
}

async function portal(t, filename) {
  const app = express(); app.use(express.json());
  const listener = await serve(t, app);
  installNodeFederation(app, filename, {config: {network: {ports: {api: listener.port}}}});
  app.get('/api/system/status', (_req, res) => res.json({radar: 'no-data', processorFresh: false}));
  app.get('/api/live-aircraft-associations', (_req, res) => res.json({associations: []}));
  return listener;
}

test('manual endpoints support portable private address families and reject public targets', () => {
  for (const host of ['10.22.3.4', '172.25.4.9', '192.168.13.4', '100.96.0.5',
    '127.0.0.1', '169.254.2.3', '::1', 'fd12::1', 'fe80::1'])
    assert.match(localNodeOrigin(host, 45321), /^http:/);
  for (const host of ['example.test', '8.8.8.8', '2001:4860:4860::8888', 'http://127.0.0.1'])
    assert.throws(() => localNodeOrigin(host, 45321), /address|addresses/);
  for (const port of [0, -1, 65536, 3.5, 'invalid'])
    assert.throws(() => localNodeOrigin('127.0.0.1', port), /port/);
  assert.throws(() => localNodeOrigin('fe80::1%en0', 3000), /Scoped IPv6/);
});

test('capability metadata describes each supported receiver without selecting resources by hardware', () => {
  const resources = nodeCapabilities().resources;
  for (const [type, count] of [['Unconfigured', 0], ['Usrp', 2], ['Kraken', 5], ['HackRF', 2], ['RspDuo', 2]]) {
    const value = nodeCapabilities({capture: {device: {type, channel_count: count}}});
    assert.deepEqual(value.receiver, {type, channel_count: count});
    assert.deepEqual(value.resources, resources);
  }
});

test('registry refuses duplicate peer identities even when aliases differ', t => {
  const filename = registryFile(t, [
    {id: 'alias-one', label: 'One', expectedIdentity: 'same-peer', url: 'http://127.0.0.1:43210'},
    {id: 'alias-two', label: 'Two', expectedIdentity: 'same-peer', url: 'http://127.0.0.1:43211'}]);
  assert.throws(() => registry(filename), /identity/i);
});

test('discovery refuses incompatible protocol versions and malformed resource or role declarations', async t => {
  const remote = await peer(t);
  const valid = remote.state.capabilities;
  for (const patch of [{protocol_version: 2}, {resources: 'tracks'}, {node_role: 'unknown'}]) {
    remote.state.capabilities = {...valid, ...patch};
    await assert.rejects(discoverLocalNode('127.0.0.1', remote.port), /compatible/);
  }
  remote.state.capabilities = valid;
  assert.equal((await discoverLocalNode('127.0.0.1', remote.port)).id, 'research-peer');
});

test('remove and re-add persists peers and discards their previous cached products', async t => {
  const remote = await peer(t), filename = registryFile(t);
  const local = await portal(t, filename);
  const body = {host: '127.0.0.1', port: remote.port, label: 'Research node'};
  assert.equal((await request(local.origin, '/api/federation/nodes', 'POST', body)).status, 201);
  assert.equal(fs.statSync(filename).mode & 0o777, 0o600);
  assert.equal(registry(filename)[0].label, body.label);
  assert.equal((await request(local.origin, '/api/federation/nodes', 'POST', body)).status, 409);
  assert.equal((await request(local.origin, '/api/federation/live-aircraft-associations')).value.associations.length, 1);
  const summary = await request(local.origin, '/api/federation/statistics-summary');
  assert.ok(summary.value.nodes.some(node => node.id === 'research-peer' && node.available));
  assert.equal((await request(local.origin, '/api/federation/nodes/research-peer', 'DELETE')).status, 200);
  assert.deepEqual(registry(filename), []);
  assert.deepEqual((await request(local.origin, '/api/federation/live-aircraft-associations')).value.associations, []);
  assert.ok(!(await request(local.origin, '/api/federation/statistics-summary')).value.nodes
    .some(node => node.id === 'research-peer'));
  remote.state.failProducts = true;
  assert.equal((await request(local.origin, '/api/federation/nodes', 'POST', body)).status, 201);
  const readded = await request(local.origin, '/api/federation/statistics-summary');
  assert.ok(readded.value.health.unhealthy_nodes.includes('research-peer'),
    'Re-added unavailable peer cannot inherit its earlier healthy statistics');
  assert.ok(!readded.value.nodes.some(node => node.id === 'research-peer'));
  const restarted = await portal(t, filename);
  assert.equal((await request(restarted.origin, '/api/federation/nodes')).value.nodes[1].label, body.label);
});

test('a pending response cannot repopulate caches after peer removal', async t => {
  const remote = await peer(t), filename = registryFile(t);
  const local = await portal(t, filename);
  await request(local.origin, '/api/federation/nodes', 'POST', {host: '127.0.0.1', port: remote.port});
  let release;
  remote.state.held = new Promise(resolve => { release = resolve; });
  const received = new Promise(resolve => remote.server.once('request', resolve));
  const pending = request(local.origin, '/api/federation/live-aircraft-associations');
  await received;
  await request(local.origin, '/api/federation/nodes/research-peer', 'DELETE');
  release();
  const result = await pending;
  assert.ok(result.status !== 200 || result.value.associations.length === 0,
    'A response from an obsolete registry must not publish removed-peer data');
  assert.deepEqual((await request(local.origin, '/api/federation/live-aircraft-associations')).value.associations, []);
});

test('existing aliases expose authenticated identity and cannot add the same peer again', async t => {
  const remote = await peer(t);
  const filename = registryFile(t, [{id: 'lab-alias', label: 'Existing node',
    expectedIdentity: 'research-peer', url: remote.origin}]);
  const local = await portal(t, filename), original = fs.readFileSync(filename, 'utf8');
  assert.equal((await request(local.origin, '/api/federation/nodes')).value.nodes[1].identity, 'research-peer');
  assert.equal((await request(local.origin, '/api/federation/nodes', 'POST', {
    host: '127.0.0.1', port: remote.port})).status, 409);
  assert.equal(fs.readFileSync(filename, 'utf8'), original);
  remote.state.identity = 'replacement-peer';
  assert.equal((await request(local.origin, '/api/federation/nodes/lab-alias/api/system/status')).status, 502);
  assert.equal(fs.readFileSync(filename, 'utf8'), original);
});

test('registry rejects a local peer alias and reserved local identities', t => {
  const previous = process.env.VECTORWARP_NODE_ID;
  t.after(() => {
    if (previous === undefined) delete process.env.VECTORWARP_NODE_ID;
    else process.env.VECTORWARP_NODE_ID = previous;
  });
  process.env.VECTORWARP_NODE_ID = 'lab-local';
  const filename = registryFile(t, [{id: 'alias', label: 'Loop',
    expectedIdentity: 'lab-local', url: 'http://127.0.0.1:43210'}]);
  assert.throws(() => installNodeFederation(express(), filename), /local identity/);
  process.env.VECTORWARP_NODE_ID = 'aggregate';
  assert.throws(() => installNodeFederation(express(), registryFile(t)), /Invalid node identity/);
});

test('multiple receiver peers and an explicit coordinator survive removal and registry reload', async t => {
  const filename = registryFile(t), local = await portal(t, filename);
  const peers = [];
  for (const [index, type] of ['Usrp', 'Kraken', 'HackRF', 'RspDuo'].entries()) {
    const remote = await peer(t, `receiver-${index}`);
    remote.state.capabilities.receiver = {type, channel_count: type === 'Kraken' ? 5 : 2};
    peers.push(remote);
  }
  const display = await peer(t, 'display-peer'); display.state.capabilities.node_role = 'coordinator';
  for (const remote of [...peers, display])
    assert.equal((await request(local.origin, '/api/federation/nodes', 'POST', {
      host: '127.0.0.1', port: remote.port})).status, 201);
  assert.equal(registry(filename).at(-1).role, 'coordinator');
  assert.deepEqual((await request(local.origin, '/api/federation/statistics-summary')).value.health.coordinator_nodes,
    ['display-peer']);
  assert.equal((await request(local.origin, '/api/federation/nodes/receiver-1/api/config', 'POST', {})).status, 405);
  await request(local.origin, '/api/federation/nodes/receiver-1', 'DELETE');
  assert.equal(registry(filename).length, 4);
  await request(local.origin, '/api/federation/nodes', 'POST', {host: '127.0.0.1', port: peers[1].port});
  const restarted = await portal(t, filename);
  assert.equal((await request(restarted.origin, '/api/federation/nodes')).value.nodes.length, 6);
  assert.equal(registry(filename).find(node => node.id === 'display-peer').role, 'coordinator');
});

test('failed discovery leaves the existing registry byte-for-byte intact', async t => {
  const remote = await peer(t), filename = registryFile(t), local = await portal(t, filename);
  await request(local.origin, '/api/federation/nodes', 'POST', {host: '127.0.0.1', port: remote.port});
  const original = fs.readFileSync(filename, 'utf8');
  const unavailable = await serve(t, (_req, res) => res.writeHead(503).end('{}'));
  for (const target of [{host: '127.0.0.1', port: unavailable.port}, {host: '8.8.8.8', port: 3000}])
    assert.equal((await request(local.origin, '/api/federation/nodes', 'POST', target)).status, 400);
  assert.equal(fs.readFileSync(filename, 'utf8'), original);
});

test('concurrent discovery cannot register duplicate peers or exceed the registry limit', async t => {
  const remote = await peer(t), filename = registryFile(t), local = await portal(t, filename);
  const add = target => request(target.origin, '/api/federation/nodes', 'POST',
    {host: '127.0.0.1', port: remote.port});
  assert.deepEqual((await Promise.all([add(local), add(local)])).map(result => result.status).sort(), [201, 409]);
  assert.equal(registry(filename).length, 1);
  const full = registryFile(t, Array.from({length: 15}, (_, index) => ({id: `existing-${index}`,
    label: `Existing ${index}`, expectedIdentity: `existing-${index}`, url: `http://127.0.0.1:${43000 + index}`})));
  const almostFull = await portal(t, full);
  assert.deepEqual((await Promise.all([add(almostFull), add(almostFull)])).map(result => result.status).sort(), [201, 409]);
  assert.equal(registry(full).length, 16);
});

test('transient peer outages stay degraded and recover after bounded cache expiry', async t => {
  let now = Date.now(); t.mock.method(Date, 'now', () => now);
  const remote = await peer(t), filename = registryFile(t), local = await portal(t, filename);
  await request(local.origin, '/api/federation/nodes', 'POST', {host: '127.0.0.1', port: remote.port});
  assert.equal((await request(local.origin, '/api/federation/statistics-summary')).value.health.nodes_available, 1);
  now += 6000; remote.state.failProducts = true;
  const stale = (await request(local.origin, '/api/federation/statistics-summary')).value;
  assert.ok(stale.health.unhealthy_nodes.includes('research-peer'));
  assert.equal(stale.nodes[0].stale, true);
  now += 31000;
  assert.equal((await request(local.origin, '/api/federation/statistics-summary')).value.health.nodes_available, 0);
  now += 6000; remote.state.failProducts = false;
  const recovered = (await request(local.origin, '/api/federation/statistics-summary')).value;
  assert.equal(recovered.health.nodes_available, 1);
  assert.ok(!recovered.health.unhealthy_nodes.includes('research-peer'));
  assert.notEqual(recovered.nodes[0].stale, true);
});
