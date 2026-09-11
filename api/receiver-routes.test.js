'use strict';

const assert = require('assert/strict');
const {installReceiverRoutes, sameReceiverOrigin} = require('./receiver-routes');
const {receiverSetupGuide} = require('./receiver-setup-guide');

function request(origin, host = '127.0.0.1:3000', body = {}) {
  return {body, method: 'POST', get: name => ({Origin: origin, Host: host,
    'X-VectorWarp-Intent': 'receiver-management-v1', 'Content-Type': 'application/json'})[name]};
}
function response() {
  return {statusCode: 200, body: null, removed: [],
    status(value) { this.statusCode = value; return this; },
    json(value) { this.body = value; return this; },
    set() { return this; }, removeHeader(name) { this.removed.push(name); }};
}

async function main() {
  const receiver = {type: 'HackRF', capabilities: {liveCompiled: false}};
  let guide = receiverSetupGuide(receiver, '/opt/vectorwarp/libexec/vectorwarp-receiver-helper');
  assert.equal(guide.length, 1);
  assert.match(guide[0].text, /build containing/);
  assert.equal(guide[0].command, undefined, 'An SDK install cannot enable an excluded backend');
  receiver.capabilities.liveCompiled = true; receiver.dependencies = {state: 'missing'};
  guide = receiverSetupGuide(receiver, '/opt/vectorwarp/libexec/vectorwarp-receiver-helper');
  assert(guide.some(item => item.command?.endsWith('enroll-packages HackRF')));
  assert(guide.some(item => /Fedora 44/.test(item.text)));
  const remoteGuide = receiverSetupGuide({type: 'Kraken', locality: 'remote', capabilities: {liveCompiled: true}}, '/fixture');
  assert(remoteGuide.some(item => /existing remote Suite/.test(item.text)));
  assert(!remoteGuide.some(item => item.command), 'Remote Suite does not acquire local service authority');
  assert.equal(sameReceiverOrigin(request('http://127.0.0.1:3000')), true);
  assert.equal(sameReceiverOrigin(request('http://127.0.0.1:4000')), false);
  assert.equal(sameReceiverOrigin(request('http://external.invalid')), false);
  assert.equal(sameReceiverOrigin(request('null')), false);
  assert.equal(sameReceiverOrigin(request(undefined)), false);
  assert.equal(sameReceiverOrigin(request('http://rebound.invalid:3000', 'rebound.invalid:3000')), false);
  const routes = {};
  const app = {get: (path, handler) => { routes[`GET ${path}`] = handler; },
    post: (path, handler) => { routes[`POST ${path}`] = handler; }};
  let discoveryCalls = 0;
  let revision = 'original';
  installReceiverRoutes(app, {
    preview: true, compiledLiveTypes: ['Kraken'],
    readDocument: () => ({revision, config: {capture: {device: {type: 'Kraken'}}}}),
    createProbes: () => { throw new Error('Preview must never construct host probes.'); },
    createManager: () => ({
      discover: async () => { discoveryCalls++; return {schemaVersion: 1, receivers: []}; },
      plan: input => ({receiverType: input.receiverType, executable: false})
    })
  });
  const first = response();
  await routes['GET /api/receivers'](request('http://127.0.0.1:3000'), first);
  assert.equal(first.body.preview, true);
  assert.equal(first.body.managementAvailable, false);
  await routes['GET /api/receivers'](request('http://127.0.0.1:3000'), response());
  assert.equal(discoveryCalls, 1);
  revision = 'changed';
  await routes['GET /api/receivers'](request('http://127.0.0.1:3000'), response());
  assert.equal(discoveryCalls, 2);
  const crossOrigin = response();
  await routes['GET /api/receivers'](request('http://external.invalid'), crossOrigin);
  assert.equal(crossOrigin.statusCode, 403);
  assert.deepEqual(crossOrigin.removed, ['Access-Control-Allow-Origin']);
  const forbidden = response();
  await routes['POST /api/receivers/plan'](request('http://127.0.0.1:3000', undefined,
    {receiverType: 'Kraken', command: 'arbitrary'}), forbidden);
  assert.equal(forbidden.statusCode, 422);
  const plan = response();
  await routes['POST /api/receivers/plan'](request('http://127.0.0.1:3000', undefined, {receiverType: 'Kraken'}), plan);
  assert.equal(plan.body.executable, false);
  assert.equal(plan.body.configRevision, 'changed');
  console.log('Receiver discovery route tests passed; trusted origins required and preview actions disabled.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
