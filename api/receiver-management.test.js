'use strict';
const assert = require('assert/strict');
const http = require('http');
const express = require('express');
const {installReceiverRoutes, sameReceiverOrigin} = require('./receiver-routes');
const {createReceiverManager} = require('./receiver-manager');

const origin = 'http://127.0.0.1:3000';
function req(body = {}, headers = {}, method = 'POST') {
  const values = {Origin: origin, Host: '127.0.0.1:3000', 'Content-Type': 'application/json',
    'X-VectorWarp-Intent': 'receiver-management-v1', ...headers};
  return {body, method, get: name => values[name]};
}
function response() {
  return {statusCode: 200, status(value) { this.statusCode = value; return this; },
    json(value) { this.body = value; return this; }, set() { return this; }, removeHeader() {}};
}
async function main() {
  let revision = 'a'.repeat(64), timestamp = 1000, authorized = false, executions = 0, hold;
  const routes = {}, requests = [];
  const helper = async request => {
    requests.push(request);
    if (request.verb === 'discover') return {ok: true, actions: [{id: 'reviewed-hackrf', receiverType: 'HackRF', kind: 'install-packages', available: true}]};
    if (request.verb === 'plan') return {ok: true, planId: 'b'.repeat(64), lifetimeSeconds: 300, status: 'awaiting-local-authorization'};
    if (!authorized) return {ok: false, code: 'LOCAL_AUTHORIZATION_REQUIRED', message: 'Authorize locally.'};
    executions++;
    if (hold) await hold;
    return {ok: true, status: 'complete', postconditionVerified: true};
  };
  const installed = installReceiverRoutes({get: (key, handler) => routes[key] = handler,
    post: (key, handler) => routes[key] = handler}, {readDocument: () => ({revision, config: {}}),
    allowedOrigins: new Set([origin]), helper, now: () => timestamp,
    createProbes: () => ({}), createManager: () => ({discover: async () => ({schemaVersion: 1, receivers: []}), plan: () => ({})})});
  async function call(path, body, headers) {
    const res = response(); await routes[path](req(body, headers), res); return res;
  }
  for (const headers of [{Origin: undefined}, {Origin: 'null'}, {Origin: 'http://127.0.0.1:4000'},
    {Origin: 'https://127.0.0.1:3000'}, {Origin: 'http://evil.invalid:3000', Host: 'evil.invalid:3000'},
    {'X-VectorWarp-Intent': undefined}, {'Content-Type': 'text/plain'}]) {
    const rejected = await call('/api/receivers/plan', {receiverType: 'HackRF', actionId: 'reviewed-hackrf'}, headers);
    assert.equal(rejected.statusCode, 403);
  }
  assert.equal(requests.length, 0, 'untrusted HTTP requests must never reach the root socket');
  assert.equal(sameReceiverOrigin(req({}, {Origin: undefined, Referer: `${origin}/settings`, 'Sec-Fetch-Site': 'same-origin'}, 'GET')), true);
  assert.equal(sameReceiverOrigin(req({}, {Origin: undefined, Referer: `${origin}/settings`, 'Sec-Fetch-Site': 'cross-site'}, 'GET')), false);
  const lan = 'http://100.64.12.34:3000';
  const lanRequest = req({}, {Host: '100.64.12.34:3000', Origin: undefined,
    Referer: `${lan}/display/configuration/`, 'Sec-Fetch-Site': undefined}, 'GET');
  assert.equal(sameReceiverOrigin(lanRequest, new Set([lan])), true,
    'Remote ordinary HTTP browser GET uses exact trusted Referer without Fetch Metadata');
  assert.equal(sameReceiverOrigin({...lanRequest, method: 'POST'}, new Set([lan])), false,
    'Missing Origin must remain forbidden on mutations');
  let plan = (await call('/api/receivers/plan', {receiverType: 'HackRF', actionId: 'reviewed-hackrf'})).body;
  assert.match(plan.nonce, /^[a-f0-9]{64}$/);
  const forged = await call('/api/receivers/execute', {nonce: 'c'.repeat(64), configRevision: revision});
  assert.equal(forged.body.code, 'PLAN_EXPIRED');
  const ungranted = await call('/api/receivers/execute', {nonce: plan.nonce, configRevision: revision});
  assert.equal(ungranted.body.code, 'LOCAL_AUTHORIZATION_REQUIRED');
  assert.equal(executions, 0);
  authorized = true;
  const complete = await call('/api/receivers/execute', {nonce: plan.nonce, configRevision: revision});
  assert.equal(complete.body.status, 'complete'); assert.equal(executions, 1);
  const replayed = await call('/api/receivers/execute', {nonce: plan.nonce, configRevision: revision});
  assert.equal(replayed.body.code, 'PLAN_EXPIRED'); assert.equal(executions, 1);
  plan = (await call('/api/receivers/plan', {receiverType: 'HackRF', actionId: 'reviewed-hackrf'})).body;
  revision = 'd'.repeat(64);
  assert.equal((await call('/api/receivers/execute', {nonce: plan.nonce, configRevision: plan.configRevision})).body.code, 'CONFIG_CHANGED');
  plan = (await call('/api/receivers/plan', {receiverType: 'HackRF', actionId: 'reviewed-hackrf'})).body;
  timestamp += 300001;
  assert.equal((await call('/api/receivers/execute', {nonce: plan.nonce, configRevision: revision})).body.code, 'PLAN_EXPIRED');
  plan = (await call('/api/receivers/plan', {receiverType: 'HackRF', actionId: 'reviewed-hackrf'})).body;
  let release; hold = new Promise(resolve => { release = resolve; });
  const pending = call('/api/receivers/execute', {nonce: plan.nonce, configRevision: revision});
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(installed.busy(), true);
  assert.equal((await call('/api/receivers/execute', {nonce: plan.nonce, configRevision: revision})).body.code, 'PLAN_EXPIRED');
  release(); await pending; assert.equal(installed.busy(), false);
  const usb = ['a', 'b'].map(serial => ({manufacturer: 'Great Scott Gadgets', product: 'HackRF One', serial}));
  const manager = createReceiverManager({probes: {usbInventory: async () => usb, dependencyInventory: async () => ({})}});
  for (const [serial, expected] of [[['a', 'b'], true], [['c', 'd'], false], [['a', 'a'], false], [['a', 'REFERENCE_DEVICE_SERIAL_NUMBER'], false]]) {
    const discovered = await manager.discover({config: {capture: {device: {type: 'HackRF', serial}}}, compiledLiveTypes: ['HackRF']});
    const receiver = discovered.receivers.find(item => item.type === 'HackRF');
    assert.equal(receiver.capabilities.detected, expected);
    assert.equal(receiver.detection.configuredIdentityMatched, expected);
  }
  // Real disposable HTTP route fixture. The Host/Referer model a Brave browser
  // on a LAN/Tailscale HTTP address, where Sec-Fetch-Site is not sent.
  const app = express(); app.use(express.json());
  app.use((request, result, next) => { result.set('Access-Control-Allow-Origin', '*'); next(); });
  installReceiverRoutes(app, {preview: true, allowedOrigins: new Set([lan]),
    readDocument: () => ({revision, config: {}}),
    createManager: () => ({discover: async () => ({schemaVersion: 1, receivers: []}), plan: () => ({})})});
  const server = app.listen(0, '127.0.0.1');
  await new Promise(resolve => server.once('listening', resolve));
  async function realRequest(method, route, headers) {
    return new Promise((resolve, reject) => {
      const request = http.request({hostname: '127.0.0.1', port: server.address().port, method, path: route,
        headers: {Host: '100.64.12.34:3000', Referer: `${lan}/display/configuration/`,
          ...(method === 'POST' ? {'Content-Type': 'application/json', 'X-VectorWarp-Intent': 'receiver-management-v1'} : {}), ...headers}}, response => {
        response.resume(); response.on('end', () => resolve({status: response.statusCode, headers: response.headers}));
      });
      request.on('error', reject); request.end(method === 'POST' ? '{}' : undefined);
    });
  }
  try {
    const get = await realRequest('GET', '/api/receivers');
    assert.equal(get.status, 200); assert.equal(get.headers['access-control-allow-origin'], undefined);
    assert.equal((await realRequest('GET', '/api/receivers', {'Sec-Fetch-Site': 'cross-site'})).status, 403);
    assert.equal((await realRequest('GET', '/api/receivers', {Host: 'attacker.invalid', Referer: 'http://attacker.invalid/page'})).status, 403);
    assert.equal((await realRequest('POST', '/api/receivers/discover')).status, 403);
    assert.equal((await realRequest('POST', '/api/receivers/discover', {Origin: lan})).status, 200);
  } finally { await new Promise(resolve => server.close(resolve)); }
  console.log('Receiver management origin/rebinding, nonce, local grant, revision, replay, deadline, lock and configured-serial tests passed.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
