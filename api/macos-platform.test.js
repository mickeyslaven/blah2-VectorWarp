'use strict';
const assert = require('assert/strict');
const {createReceiverProbes} = require('./receiver-probes');
const {usbFromSystemProfiler} = require('./macos-receiver-probes');
const {receiverSetupGuide} = require('./receiver-setup-guide');
const {createGpuSetupStatus} = require('./gpu-setup');

(async () => {
  const inventory = JSON.stringify({SPUSBDataType: [{_name: 'USB bus', _items: [
    {_name: 'HackRF One', product_id: '0x6089', manufacturer: 'Great Scott Gadgets', serial_num: 'serial'},
    {_name: 'RSPduo', product_id: '0x3020', manufacturer: 'SDRplay'}]}]});
  assert.equal(usbFromSystemProfiler(inventory).length, 2);
  assert.equal(usbFromSystemProfiler(inventory)[0].serial, 'serial');
  assert.throws(() => usbFromSystemProfiler('{"SPUSBDataType":{}}'), /inventory/);
  assert.throws(() => usbFromSystemProfiler('x'.repeat(262145)), /limit/);
  const calls = [];
  const probes = createReceiverProbes({}, {platform: 'darwin', env: {}, readHeader: async () => '#define SDRPLAY_API_VERSION 3.15',
    readable: async file => ['/opt/homebrew/lib/libhackrf.dylib', '/usr/local/include/sdrplay_api.h',
      '/usr/local/lib/libsdrplay_api.dylib'].includes(file),
    run: async (file, args) => { calls.push(file); assert.deepEqual(args, ['SPUSBDataType', '-json']); return inventory; }});
  assert.equal((await probes.usbInventory({})).length, 2);
  const dependencies = await probes.dependencyInventory({});
  assert.equal(dependencies.HackRF.state, 'installed');
  assert.equal(dependencies.RspDuo.state, 'installed');
  assert.equal(dependencies.Usrp.state, 'unknown');
  assert.equal((await probes.serviceStatus({serviceId: 'sdrplay-api'})).state, 'unknown');
  assert.deepEqual(calls, ['/usr/sbin/system_profiler']);
  const missing = createReceiverProbes({}, {platform: 'darwin', env: {}, readable: async () => false});
  assert.equal((await missing.dependencyInventory({})).RspDuo.state, 'unknown', 'Unknown private SDK paths are not proof of absence.');
  const custom = createReceiverProbes({}, {platform: 'darwin', env: {
    BLAH2_SDRPLAY_INCLUDE_DIR: '/custom/include', BLAH2_SDRPLAY_LIBRARY: '/custom/sdk.dylib'},
    readHeader: async () => '#define SDRPLAY_API_VERSION (float)(3.15) // vendor header form',
    readable: async file => ['/custom/include/sdrplay_api.h', '/custom/sdk.dylib'].includes(file)});
  assert.equal((await custom.dependencyInventory({})).RspDuo.state, 'installed');
  const vendorLayout = createReceiverProbes({}, {platform: 'darwin', env: {},
    readHeader: async () => '#define SDRPLAY_API_VERSION (float)(3.15)',
    readable: async file => ['/usr/local/include/sdrplay_api.h',
      '/usr/local/lib/libsdrplay_api.so.3.15'].includes(file)});
  assert.equal((await vendorLayout.dependencyInventory({})).RspDuo.state, 'installed',
    'Detect the official macOS SDK filename as well as dylib aliases.');
  const outdated = createReceiverProbes({}, {platform: 'darwin', env: {}, readable: async () => true,
    readHeader: async () => '#define SDRPLAY_API_VERSION 3.07'});
  assert.equal((await outdated.dependencyInventory({})).RspDuo.state, 'unknown',
    'An older header and library filename cannot establish the required SDK version.');
  for (const type of ['Kraken', 'RspDuo', 'Usrp', 'HackRF']) {
    const steps = receiverSetupGuide({type, capabilities: {liveCompiled: false}}, '/unused', {platform: 'darwin'});
    assert.ok(steps.every(step => !step.command));
    assert.ok(steps.some(step => /replay/.test(step.text)));
    if (type === 'RspDuo') assert.ok(steps.some(step => step.link === 'https://sdrplay.com/hardware-api/'));
  }
  const routes = {};
  require('./receiver-routes').installReceiverRoutes({get: (key, handler) => routes[key] = handler,
    post: (key, handler) => routes[key] = handler}, {platform: 'darwin',
    readDocument: () => ({revision: 'test', config: {}}),
    helper: () => { throw new Error('Linux broker invoked'); },
    macManagement: {discover: () => ({available: false, actions: [], code: 'HOMEBREW_REQUIRED'}), plan: () => null, execute: () => { throw new Error('unexpected'); }},
    createProbes: () => ({}), createManager: () => ({discover: async () => ({receivers: []})})});
  const response = {set() {}, removeHeader() {}, vary() {}, json(value) { this.value = value; }};
  await routes['/api/receivers']({method: 'GET', get: name => ({Host: '127.0.0.1:3000',
    Origin: 'http://127.0.0.1:3000'})[name]}, response);
  assert.equal(response.value.managementAvailable, false);
  assert.equal(response.value.management.code, 'HOMEBREW_REQUIRED');
  const managedRoutes = {};
  let helperCalls = 0; let executed = 0;
  require('./receiver-routes').installReceiverRoutes({get: (key, handler) => managedRoutes[key] = handler,
    post: (key, handler) => managedRoutes[key] = handler}, {platform: 'darwin',
    readDocument: () => ({revision: 'revision', config: {}}), helper: () => { helperCalls++; throw new Error('Linux broker invoked'); },
    macManagement: {discover: () => ({available: true, actions: [{id: 'macos-install-uhd', receiverType: 'Usrp', available: true}]}),
      plan: id => id === 'macos-install-uhd' ? {ok: true, status: 'ready', requiresAuthorization: false} : null,
      execute: async id => { executed++; return {ok: true, message: id}; }},
    createProbes: () => ({}), createManager: () => ({discover: async () => ({receivers: [{type: 'Usrp', label: 'USRP', capabilities: {}}]})})});
  const makeResponse = () => ({statusCode: 200, set() {}, removeHeader() {}, vary() {}, status(code) { this.statusCode = code; return this; }, json(value) { this.value = value; return this; }});
  const mutation = body => ({body, method: 'POST', protocol: 'http', get: name => ({Host: '127.0.0.1:3000', Origin: 'http://127.0.0.1:3000', 'X-VectorWarp-Intent': 'receiver-management-v1', 'Content-Type': 'application/json'})[name]});
  const macPlan = makeResponse(); await managedRoutes['/api/receivers/plan'](mutation({receiverType: 'Usrp', actionId: 'macos-install-uhd'}), macPlan);
  assert.equal(macPlan.value.requiresAuthorization, false); assert.equal(macPlan.value.authorizationCommand, undefined);
  const macExecute = makeResponse(); await managedRoutes['/api/receivers/execute'](mutation({nonce: macPlan.value.nonce, configRevision: 'revision'}), macExecute);
  assert.equal(macExecute.value.ok, true); assert.equal(executed, 1); assert.equal(helperCalls, 0, 'macOS must never invoke the Linux receiver broker');
  const setup = await createGpuSetupStatus({platform: 'darwin', execute: () => { throw new Error('Linux helper invoked'); }})();
  assert.equal(setup.state, 'unqualified');
  assert.equal(setup.qualification, 'not-run');
  assert.ok(!setup.command && !setup.accessCommand);
  console.log('macOS receiver/GPU platform fixtures passed; no hardware or vendor service accessed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
