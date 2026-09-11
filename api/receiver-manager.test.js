'use strict';

const assert = require('assert');
const {RECEIVER_TYPES, createReceiverManager, getSettingsMapping,
  planReceiverSetup} = require('./receiver-manager.js');

const baseConfig = {capture: {fs: 2400000, fc: 204640000, device: {
  type: 'Kraken', channel_count: 5,
  heimdall: {host: '127.0.0.1', port: 8091, control_port: 8092}
}}};

async function expectReject(promise, pattern) {
  try { await promise; }
  catch (error) {
    assert.match(error.message, pattern);
    assert.equal(error.status, 422);
    return;
  }
  assert.fail('Expected rejection.');
}

(async () => {
  const calls = [];
  const manager = createReceiverManager({timeoutMs: 100, now: () => 12345,
    probes: {
      usbInventory: async () => [
        // Generic RTL descriptors must never be promoted to Kraken identity.
        {manufacturer: 'Realtek', product: 'RTL2838UHIDIR', serial: '1'},
        {manufacturer: 'SDRplay', product: 'RSPduo', serial: 'rsp'},
        {manufacturer: 'Ettus Research', product: 'USRP B210', serial: 'usrp'},
        {manufacturer: 'Great Scott Gadgets', product: 'HackRF One', serial: 'a'},
        {manufacturer: 'Great Scott Gadgets', product: 'HackRF One', serial: 'b'},
        // A spoofed product without the expected manufacturer is not enough.
        {manufacturer: 'Unrelated', product: 'HackRF One', serial: 'c'}
      ],
      dependencyInventory: async () => ({
        Kraken: {state: 'installed', version: 'v2'},
        RspDuo: {state: 'missing'},
        Usrp: {state: 'installed', version: '4.8'},
        HackRF: {state: 'installed'}
      }),
      configuredUpstreamStatus: async endpoint => {
        calls.push(['upstream', endpoint]);
        return {available: true, matched: true,
          capabilities: ['get_status', 'set_frequency', 'set_num_elements']};
      },
      serviceStatus: async target => {
        calls.push(['service', target]);
        return {state: 'running'};
      }
    }});
  const result = await manager.discover({config: baseConfig,
    compiledLiveTypes: [...RECEIVER_TYPES]});
  assert.equal(result.schemaVersion, 1);
  assert.equal(result.observedAt, 12345);
  assert.equal(result.configuredType, 'Kraken');
  assert.deepEqual(calls, [
    ['upstream', {host: '127.0.0.1', dataPort: 8091, controlPort: 8092}],
    ['service', {serviceId: 'kraken-suite-v2'}]
  ]);
  const byType = Object.fromEntries(result.receivers.map(item => [item.type, item]));
  assert.equal(byType.Kraken.capabilities.detected, true);
  assert.equal(byType.Kraken.capabilities.configured, true);
  assert.equal(byType.Kraken.locality, 'local');
  assert.equal(byType.Kraken.managedService.state, 'running');
  assert.equal(byType.Kraken.managedService.controlAvailable, false);
  assert.deepEqual(byType.Kraken.detection.evidence,
    ['configured-upstream-telemetry']);
  assert.equal(byType.RspDuo.capabilities.detected, true);
  assert.equal(byType.RspDuo.capabilities.possible, false,
    'A known missing runtime is a blocker even when hardware is present');
  assert.equal(byType.Usrp.capabilities.detected, true);
  assert.equal(byType.HackRF.capabilities.detected, true);
  assert.match(byType.HackRF.detection.evidence[0], /^2 matching/);
  assert.equal(result.errors.length, 0);

  const fsMapping = getSettingsMapping('Kraken').find(item =>
    item.configField === 'capture.fs');
  assert.equal(fsMapping.direction, 'upstream-authoritative-mismatch-block');
  assert.equal(fsMapping.controlOperation, null);
  const frequencyMapping = getSettingsMapping('Kraken').find(item =>
    item.configField === 'capture.fc');
  assert.equal(frequencyMapping.requiredCapability, null);
  assert.equal(frequencyMapping.capabilityAdvertised, false);
  assert.equal(frequencyMapping.commandContract,
    'inspected-suite-v2-newline-json');
  assert.ok(getSettingsMapping('Usrp').every(item =>
    item.direction === 'direct-tuning' && item.confirmation === 'unsupported-readback' &&
    item.hardwareReadback === false));

  const readyPlan = manager.plan({receiverType: 'Kraken'}, result);
  assert.equal(readyPlan.executable, false);
  assert.equal(readyPlan.actions.find(item => item.id === 'upstream-service').status,
    'not-required');
  assert.equal(readyPlan.actions.find(item => item.id === 'configure').execution,
    'unprivileged-existing-config-api');
  assert.deepEqual(readyPlan.progressStates,
    ['pending', 'running', 'complete', 'failed', 'blocked']);

  const rspPlan = manager.plan({receiverType: 'RspDuo'}, result);
  assert.ok(rspPlan.errors.some(error =>
    error.code === 'LICENSE_ACCEPTANCE_REQUIRED'));
  assert.equal(rspPlan.actions.find(item => item.id === 'dependency').execution,
    'unsupported');
  assert.equal(rspPlan.actions.find(item => item.id === 'upstream-service').status,
    'blocked');
  assert.equal(rspPlan.actions.find(item => item.id === 'upstream-service').target,
    'sdrplay-api');

  let remoteServiceCalled = false;
  const remoteManager = createReceiverManager({timeoutMs: 100, probes: {
    usbInventory: async () => [], dependencyInventory: async () => ({}),
    configuredUpstreamStatus: async endpoint => {
      assert.deepEqual(endpoint,
        {host: 'receiver.example', dataPort: 8091, controlPort: 8092});
      return {available: false, message: 'not reachable'};
    },
    serviceStatus: async () => { remoteServiceCalled = true; return {state: 'running'}; }
  }});
  const remoteConfig = JSON.parse(JSON.stringify(baseConfig));
  remoteConfig.capture.device.heimdall.host = 'receiver.example';
  const remote = await remoteManager.discover({config: remoteConfig,
    compiledLiveTypes: ['Kraken']});
  assert.equal(remoteServiceCalled, false,
    'Remote hosts must not be queried through the local service adapter');
  const remoteKraken = remote.receivers.find(item => item.type === 'Kraken');
  assert.equal(remoteKraken.locality, 'remote');
  assert.equal(remoteKraken.managedService.state, 'unknown');
  assert.equal(remoteKraken.upstream.availability, 'unavailable');
  const remotePlan = planReceiverSetup({receiverType: 'Kraken'}, remote);
  assert.ok(remotePlan.errors.some(error => error.code === 'REMOTE_RIGHTS_REQUIRED'));
  assert.equal(remotePlan.actions.find(item => item.id === 'dependency').kind,
    'verify-dependency');

  const availableRemoteManager = createReceiverManager({timeoutMs: 100, probes: {
    usbInventory: async () => [], dependencyInventory: async () => ({}),
    configuredUpstreamStatus: async () => ({available: true,
      capabilities: ['get_status']})
  }});
  const availableRemote = await availableRemoteManager.discover({config: remoteConfig,
    compiledLiveTypes: ['Kraken']});
  const availableRemotePlan = availableRemoteManager.plan({receiverType: 'Kraken'},
    availableRemote);
  assert.equal(availableRemote.receivers.find(item => item.type === 'Kraken')
    .dependencies.state, 'installed');
  assert.equal(availableRemotePlan.actions.find(item => item.id === 'upstream-service')
    .status, 'not-required');
  assert.ok(!availableRemotePlan.errors.some(error =>
    error.code === 'REMOTE_RIGHTS_REQUIRED'));

  const directCalls = [];
  const directManager = createReceiverManager({timeoutMs: 100, probes: {
    usbInventory: async () => [], dependencyInventory: async () => ({}),
    configuredUpstreamStatus: async () => { directCalls.push('upstream'); },
    serviceStatus: async () => { directCalls.push('service'); }
  }});
  const direct = await directManager.discover({config: {capture: {device: {
    type: 'Usrp', address: 'addr=192.168.10.2'}}}, compiledLiveTypes: ['Usrp']});
  assert.deepEqual(directCalls, [],
    'Direct receivers must not invoke upstream or service probes');
  assert.equal(direct.receivers.find(item => item.type === 'Usrp').locality,
    'remote');
  assert.ok(direct.errors.length === 0);

  let rspServiceTarget = null;
  const rspManager = createReceiverManager({timeoutMs: 100, probes: {
    usbInventory: async () => [],
    dependencyInventory: async () => ({RspDuo: {state: 'installed'}}),
    serviceStatus: async target => { rspServiceTarget = target; return {state: 'running'}; }
  }});
  const rsp = await rspManager.discover({config: {capture: {device: {
    type: 'RspDuo'}}}, compiledLiveTypes: ['RspDuo']});
  assert.deepEqual(rspServiceTarget, {serviceId: 'sdrplay-api'});
  const selectedRsp = rsp.receivers.find(item => item.type === 'RspDuo');
  assert.equal(selectedRsp.managedService.state, 'running');
  assert.equal(rspManager.plan({receiverType: 'RspDuo'}, rsp).actions
    .find(item => item.id === 'upstream-service').status, 'not-required');

  const uncertainUsrp = await directManager.discover({config: {capture: {device: {
    type: 'Usrp', address: 'radio.example'}}}, compiledLiveTypes: ['Usrp']});
  assert.equal(uncertainUsrp.receivers.find(item => item.type === 'Usrp').locality,
    'unknown', 'A bare UHD hostname is not proof of local or remote transport');

  const hostileLoopback = JSON.parse(JSON.stringify(baseConfig));
  hostileLoopback.capture.device.heimdall.host = '127.evil.example';
  const hostileResult = await remoteManager.discover({config: hostileLoopback,
    compiledLiveTypes: ['Kraken']});
  assert.equal(hostileResult.receivers.find(item => item.type === 'Kraken').locality,
    'remote', 'A hostname beginning with 127 must not pass the loopback check');

  const partial = createReceiverManager({timeoutMs: 20, probes: {
    usbInventory: async () => new Promise(() => {}),
    dependencyInventory: async () => ({HackRF: {state: 'not-a-state'}})
  }});
  const boundedAt = Date.now();
  const bounded = await partial.discover({config: {capture: {device: {
    type: 'HackRF'}}}, compiledLiveTypes: []});
  assert.ok(Date.now() - boundedAt < 1000, 'Probe deadline must be bounded');
  assert.ok(bounded.errors.some(error => error.code === 'PROBE_TIMEOUT'));
  assert.ok(bounded.errors.some(error => error.code === 'INVALID_PROBE_RESULT'));
  const hackrf = bounded.receivers.find(item => item.type === 'HackRF');
  assert.equal(hackrf.capabilities.liveCompiled, false);
  assert.equal(hackrf.detection.state, 'unknown',
    'A failed USB probe is not evidence that hardware is absent');
  assert.equal(hackrf.dependencies.state, 'unknown');
  const blocked = partial.plan({receiverType: 'HackRF'}, bounded);
  assert.ok(blocked.errors.some(error => error.code === 'BACKEND_NOT_COMPILED'));
  assert.ok(blocked.errors.some(error => error.code === 'INSTALL_ADAPTER_UNAVAILABLE'));

  const preview = await createReceiverManager({timeoutMs: 20}).discover({
    config: {capture: {device: {type: 'HackRF'}}}, compiledLiveTypes: ['HackRF']});
  for (const item of preview.receivers)
    assert.equal(item.detection.state, 'unknown',
      'Absent preview probes must never become fabricated absence evidence');

  await expectReject(manager.discover({config: {}, compiledLiveTypes: ['RTL-SDR']}),
    /compiledLiveTypes/);
  await expectReject(manager.discover({config: {}, compiledLiveTypes: [], extra: true}),
    /request\.extra/);
  await expectReject(manager.discover({config: {capture: {device: {
    type: 'RTL-SDR'}}}, compiledLiveTypes: []}), /not supported/);
  assert.throws(() => planReceiverSetup({receiverType: 'Usrp', locality: 'lan'}, result),
    /local or remote/);
  assert.throws(() => planReceiverSetup({receiverType: 'HackRF', locality: 'remote'}, result),
    /remote capture is not supported/);
  assert.throws(() => directManager.plan({receiverType: 'Usrp'}, uncertainUsrp),
    /locality is required/);
  assert.throws(() => createReceiverManager({timeoutMs: 6000}), /10 through 5000/);
  assert.throws(() => createReceiverManager({probes: {shell: () => {}}}),
    /options\.probes\.shell/);

  console.log('Read-only receiver discovery and setup-plan tests passed.');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
