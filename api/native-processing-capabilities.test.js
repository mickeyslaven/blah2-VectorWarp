'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');
const {parseNativeProcessingCapabilities, readNativeProcessingCapabilities,
  readGpuDriverStatus, enrichNativeProcessingCapabilities, selectResidentGpuDevice} =
  require('./native-processing-capabilities');

assert.deepEqual(parseNativeProcessingCapabilities(
  '{"schema":1,"portableGpu":true,"residentGpu":true,"widebandGpu":false,"reason":""}\n'),
{schema: 1, portableGpu: true, residentGpu: true, widebandGpu: false, reason: ''});
for (const report of ['', '{}', '{"schema":2,"portableGpu":true,"residentGpu":true,"widebandGpu":true}',
  '{"schema":1,"portableGpu":true,"residentGpu":1,"widebandGpu":false}',
  '{"schema":1,"portableGpu":true,"residentGpu":true,"widebandGpu":false,"reason":"","device":"x"}'])
  assert.throws(() => parseNativeProcessingCapabilities(report));

(async () => {
  const available = await readNativeProcessingCapabilities({executable: '/test/blah2',
    execFile(file, args, options, callback) {
      assert.equal(file, '/test/blah2');
      assert.deepEqual(args, ['--processing-capabilities']);
      callback(null, '{"schema":1,"portableGpu":true,"residentGpu":true,"widebandGpu":true,"reason":""}\n');
    }});
  assert.deepEqual(available,
    {schema: 1, portableGpu: true, residentGpu: true, widebandGpu: true, reason: ''});
  const failed = await readNativeProcessingCapabilities({
    execFile(_file, _args, _options, callback) {
      callback(new Error('missing'), '');
    }});
  assert.deepEqual(failed,
    {schema: 1, portableGpu: false, residentGpu: false, widebandGpu: false,
      reason: 'native-capability-query-failed'});
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-capability-'));
  const executable = path.join(directory, 'blah2');
  for (const name of ['blah2', 'blah2-gpu-worker', 'blah2-gpu-vulkan.so',
    'blah2-gpu-wideband-vulkan.so', 'blah2-gpu-resident-vulkan.so'])
    fs.writeFileSync(path.join(directory, name), 'fixture');
  const driverStatus = {version: 1, available: true, qualification: 'not-run',
    devices: [{id: '1:2:0', name: 'GPU', shaderFloat64: true}]};
  const mixedDriver = {...driverStatus, devices: [
    {id: '9:2:0', name: 'second FP64', shaderFloat64: true},
    {id: '1:2:1', name: 'first FP64', shaderFloat64: true},
    {id: '0:2:2', name: 'FP32', shaderFloat64: false}]};
  assert.equal(selectResidentGpuDevice(mixedDriver), '1:2:1');
  assert.equal(selectResidentGpuDevice({...mixedDriver, devices: [...mixedDriver.devices].reverse()}, ''), '1:2:1');
  assert.equal(selectResidentGpuDevice(mixedDriver, '9:2:0'), '9:2:0');
  assert.throws(() => selectResidentGpuDevice(mixedDriver, 'missing'), /selected-gpu-device-unavailable/);
  assert.throws(() => selectResidentGpuDevice(mixedDriver, '0:2:2'), /lacks-shaderFloat64/);
  assert.throws(() => selectResidentGpuDevice({...driverStatus, devices: []}), /no-device-with-required-features/);
  assert.throws(() => selectResidentGpuDevice({...mixedDriver, devices: [mixedDriver.devices[2]]}), /no-device-with-required-features/);
  assert.throws(() => selectResidentGpuDevice({...mixedDriver, devices: [mixedDriver.devices[0], mixedDriver.devices[0]]}), /invalid-gpu-device-report/);
  for (const deviceSelector of ['missing', '0:2:2']) {
    const denied = enrichNativeProcessingCapabilities(available, {executable,
      driverStatus: mixedDriver, deviceSelector});
    assert.equal(denied.stages.resident.selectable, false);
    assert.equal(denied.stages.portable.selectable, true);
  }
  assert.equal(enrichNativeProcessingCapabilities(available, {executable,
    driverStatus: mixedDriver, deviceSelector: 'auto'}).stages.resident.selectable, true);
  const queriedDriver = await readGpuDriverStatus({executable,
    execFile(file, args, options, callback) {
      assert.equal(file, path.join(directory, 'blah2-gpu-worker'));
      assert.deepEqual(args, ['--driver-status']);
      callback(null, JSON.stringify(driverStatus));
    }});
  assert.deepEqual(queriedDriver, driverStatus);
  const standalone = path.join(directory, 'clean standalone');
  fs.mkdirSync(path.join(standalone, 'bin'), {recursive: true});
  const standaloneExecutable = path.join(standalone, 'bin/blah2');
  fs.writeFileSync(standaloneExecutable, 'fixture');
  const icd = path.join(standalone, 'share/vulkan/icd.d/MoltenVK_icd.json');
  fs.mkdirSync(path.dirname(icd), {recursive: true});
  const queryStandalone = (platform, expectedIcd) => readGpuDriverStatus({
    executable: standaloneExecutable, platform,
    execFile(file, args, options, callback) {
      assert.equal(file, path.join(standalone, 'bin/blah2-gpu-worker'));
      assert.deepEqual(args, ['--driver-status']);
      assert.deepEqual(options.env, {PATH: '/usr/sbin:/usr/bin:/sbin:/bin', LANG: 'C', LC_ALL: 'C',
        ...(expectedIcd ? {VK_DRIVER_FILES: expectedIcd, VK_ICD_FILENAMES: expectedIcd} : {})});
      assert.equal(options.cwd, '/');
      callback(null, JSON.stringify(driverStatus));
    }});
  await queryStandalone('darwin'); // Missing bundled ICD must not inherit ambient paths.
  fs.writeFileSync(icd, '{}');
  await queryStandalone('darwin', icd); // No Homebrew path or ambient environment is needed.
  await queryStandalone('linux');
  fs.unlinkSync(icd);
  fs.mkdirSync(icd);
  await queryStandalone('darwin'); // A directory is not a packaged driver manifest.
  fs.rmdirSync(icd);
  fs.symlinkSync(path.join(directory, 'blah2'), icd);
  await queryStandalone('darwin'); // Do not trust an external symlink.
  fs.unlinkSync(icd);
  fs.rmdirSync(path.dirname(icd));
  const escapedIcdDirectory = path.join(directory, 'external icd');
  fs.mkdirSync(escapedIcdDirectory);
  fs.writeFileSync(path.join(escapedIcdDirectory, 'MoltenVK_icd.json'), '{}');
  fs.symlinkSync(escapedIcdDirectory, path.dirname(icd));
  await queryStandalone('darwin'); // Nor a symlinked ancestor outside this runtime.
  const failedDriver = {version: 1, available: false, qualification: 'not-run',
    devices: [], error: 'module could not load'};
  assert.deepEqual(await readGpuDriverStatus({execFile(_f, _a, _o, callback) {
    callback(new Error('exit 1'), JSON.stringify(failedDriver));
  }}), failedDriver);
  const frame = {fresh: true, processorGeneration: '2:3:4', nCpi: 6, timestamp: 100,
    captureSamplePosition: {available: true, epoch: 7, firstSample: 201000, endSample: 241000}};
  const exactReceipt = (epoch, first, end) => ({known: true, available: true,
    frameBound: true, sampleInterval: 'half-open', provenance: 'processor_stage_receipt',
    gpuExecuted: true, cpuExecuted: false, captureEpochExact: epoch,
    firstInputSampleExact: first, endInputSampleExact: end});
  const exactRuntime = (epoch, first, end) => ({...frame,
    captureSamplePosition: {available: true, epoch: Number(epoch),
      firstSample: Number(first), endSample: Number(end)},
    acceleration: {active: 'vulkan-resident', state: 'ready', device: 'gpu0',
      gpuExecuted: true, execution: exactReceipt(epoch, first, end)}});
  for (const [epoch, first, end] of [
    ['0', '201000', '241000'],
    ['9007199254740993', '9007199254740995', '9007199254780995'],
    ['18446744073709551615', '18446744073709551613', '18446744073709551615']]) {
    const runtime = exactRuntime(epoch, first, end);
    const value = enrichNativeProcessingCapabilities(available, {executable, driverStatus, runtime});
    assert.equal(value.frame.bound, true);
    assert.equal(value.stages.resident.executed, true);
    assert.equal(value.frame.captureSamplePosition.epochExact, epoch);
    assert.equal(value.frame.captureSamplePosition.firstSampleExact, first);
    assert.equal(value.frame.captureSamplePosition.endSampleExact, end);
    for (const key of ['epoch', 'firstSample', 'endSample'])
      if (!Number.isSafeInteger(runtime.captureSamplePosition[key]))
        assert.equal(value.frame.captureSamplePosition[key], null, 'Never publish rounded identities');
    assert.doesNotThrow(() => JSON.stringify(value));
  }
  const validExact = exactRuntime('0', '201000', '241000');
  for (const mutate of [
    runtime => { runtime.fresh = false; },
    runtime => { runtime.processorGeneration = ''; },
    runtime => { runtime.captureSamplePosition.epoch = 1; },
    runtime => { runtime.clutterAcceleration = {execution: exactReceipt('1', '201000', '241000')}; },
    ...['-1', '+1', '01', '1e3', '18446744073709551616', '9'.repeat(1000)].map(value =>
      runtime => { runtime.acceleration.execution.captureEpochExact = value; }),
    runtime => { runtime.acceleration.execution.endInputSampleExact = '201000'; },
    runtime => { runtime.acceleration.execution.frameBound = false; },
    runtime => { runtime.acceleration.execution.provenance = 'saved-config'; },
  ]) {
    const runtime = JSON.parse(JSON.stringify(validExact)); mutate(runtime);
    const value = enrichNativeProcessingCapabilities(available, {executable, driverStatus, runtime});
    assert.equal(value.frame.bound, false);
    assert.equal(value.stages.resident.executed, false);
    assert.equal(value.stages.resident.qualified, 'not-run');
  }
  const enriched = enrichNativeProcessingCapabilities(available, {executable,
    driverStatus,
    requested: 'gpu-resident', runtime: {
      ...frame,
      acceleration: {active: 'vulkan-resident', state: 'ready', device: 'gpu0',
        reason: '', gpuExecuted: true},
      widebandAcceleration: {active: 'vulkan-wideband', state: 'ready', device: 'gpu0',
        reason: '', gpuExecuted: true}}});
  assert.deepEqual(enriched.stages.resident, {compiled: true, built: true, packaged: true,
    loadable: true, discovered: true, selectable: true,
    qualified: 'ready', selected: true,
    executed: true, reason: '', device: 'gpu0', devices: ['gpu0'], eligibleDevices: ['1:2:0'],
    executionMembers: [{stage: 'ambiguity', backend: 'vulkan-resident', device: 'gpu0',
      state: 'ready', reason: '', selected: true, executedBackend: 'vulkan-resident',
      executed: true, execution: null}]});
  assert.equal(enriched.frame.bound, true);
  assert.equal(enriched.stages.portable.selected, false);
  assert.equal(enriched.stages.portable.executed, false);
  assert.equal(enriched.stages.portable.qualified, 'not-run');
  assert.equal(enriched.stages.resident.selectable, true);
  assert.equal(enriched.stages.wideband.executed, true);
  const gpuTelemetry = (backend, device = 'gpu0') => ({active: backend, state: 'ready',
    device, reason: '', gpuExecuted: true, execution: exactReceipt('7', '201000', '241000')});
  const cpuMember = {laneId: 'cpu-lane', backend: 'cpu', device: '', state: 'selected', reason: '',
    execution: {...exactReceipt('7', '201000', '241000'), gpuExecuted: false, cpuExecuted: true}};
  const gpuMember = backend => ({laneId: 'gpu-lane', backend, device: 'gpu0', state: 'ready', reason: '',
    execution: exactReceipt('7', '201000', '241000')});
  const resolveRuntime = runtime => enrichNativeProcessingCapabilities(available,
    {executable, driverStatus, runtime: {...frame, ...runtime}});
  // Clutter and ambiguity remain independent consumers of the portable module.
  const clutterOnly = resolveRuntime({acceleration: {active: 'cpu', state: 'fallback'},
    clutterAcceleration: gpuTelemetry('vulkan')});
  assert.equal(clutterOnly.stages.portable.selected, true);
  assert.equal(clutterOnly.stages.portable.executed, true);
  assert.equal(clutterOnly.stages.portable.qualified, 'ready');
  assert.equal(clutterOnly.stages.portable.executionMembers[0].stage, 'clutter');
  const widebandRetry = resolveRuntime({widebandAcceleration: {...gpuTelemetry('cpu'),
    gpuExecutionBackend: 'vulkan-wideband', state: 'fallback',
    execution: {...exactReceipt('7', '201000', '241000'), cpuExecuted: true}}});
  assert.equal(widebandRetry.stages.wideband.selected, false);
  assert.equal(widebandRetry.stages.wideband.executed, true);
  assert.equal(widebandRetry.stages.wideband.qualified, 'fallback');
  assert.equal(widebandRetry.stages.portable.executed, false);
  assert.equal(widebandRetry.stages.resident.executed, false);
  for (const [backend, stage] of [['vulkan', 'portable'], ['vulkan-resident', 'resident']]) {
    const rejected = {...gpuTelemetry('cpu'), state: 'fallback',
      gpuExecutionBackend: backend, reason: 'GPU output rejected; CPU retry',
      execution: {...exactReceipt('7', '201000', '241000'), cpuExecuted: true}};
    for (const telemetry of [rejected, {...rejected, members: [
      {...gpuMember('cpu'), executedBackend: backend, state: 'fallback',
        reason: rejected.reason, execution: rejected.execution}]}]) {
      const fallback = resolveRuntime({acceleration: telemetry}).stages[stage];
      assert.equal(fallback.selected, false, 'CPU retry must not select a GPU backend');
      assert.equal(fallback.executed, true, 'Completed GPU attempt must survive CPU retry');
      assert.equal(fallback.qualified, 'fallback');
      assert.equal(fallback.reason, rejected.reason);
      assert.equal(fallback.executionMembers[0].executedBackend, backend);
      for (const patch of [{fresh: false}, {processorGeneration: ''}])
        assert.equal(resolveRuntime({...patch, acceleration: telemetry}).stages[stage].executed, false);
    }
    for (const patch of [{gpuExecuted: false}, {gpuExecutionBackend: ''}, {device: ''},
      {execution: {...rejected.execution, gpuExecuted: false}}])
      assert.equal(resolveRuntime({acceleration: {...rejected, ...patch}}).stages[stage].executed, false);
    const telemetry = {...gpuTelemetry('mixed'), members: [cpuMember, gpuMember(backend)]};
    const mixed = resolveRuntime({acceleration: telemetry});
    assert.equal(mixed.stages[stage].selected, true);
    assert.equal(mixed.stages[stage].executed, true);
    assert.equal(mixed.stages[stage].qualified, 'ready');
    assert.equal(mixed.stages[stage].device, 'gpu0');
    assert.equal(mixed.stages[stage].executionMembers[0].laneId, 'gpu-lane');
    for (const mutation of [
      runtime => { runtime.fresh = false; },
      runtime => { runtime.captureSamplePosition = {...frame.captureSamplePosition, available: false}; },
      runtime => { runtime.acceleration.members = [cpuMember]; },
      runtime => { runtime.acceleration.members = []; },
      runtime => { runtime.acceleration.members[1].execution.captureEpochExact = '6'; },
      runtime => { runtime.acceleration.members[1].execution.endInputSampleExact = '241001'; },
      runtime => { runtime.acceleration.members[1].execution.frameBound = false; },
      runtime => { runtime.acceleration.members[1].execution.gpuExecuted = false; },
    ]) {
      const runtime = {acceleration: JSON.parse(JSON.stringify(telemetry))}; mutation(runtime);
      assert.equal(resolveRuntime(runtime).stages[stage].executed, false);
    }
  }
  const both = resolveRuntime({acceleration: {...gpuTelemetry('mixed'),
    members: [cpuMember, gpuMember('vulkan'), {...gpuMember('vulkan-resident'), laneId: 'resident-lane'}]}});
  assert.equal(both.stages.portable.executed, true);
  assert.equal(both.stages.resident.executed, true);
  const pi = resolveRuntime({acceleration: gpuTelemetry('vulkan+cpu')});
  assert.equal(pi.stages.portable.selected, false);
  assert.equal(pi.stages.portable.executed, false);
  assert.equal(pi.stages.portable.qualified, 'not-run');
  const legacyMember = resolveRuntime({acceleration: {...gpuTelemetry('mixed'),
    members: [cpuMember, gpuMember('vulkan+cpu')]}});
  assert.equal(legacyMember.stages.portable.selected, false);
  assert.equal(legacyMember.stages.portable.executed, false);
  assert.equal(legacyMember.stages.portable.qualified, 'not-run');
  for (const patch of [{gpuExecuted: false}, {device: ''},
    {execution: {...exactReceipt('7', '201000', '241000'), gpuExecuted: false}}])
    assert.equal(resolveRuntime({acceleration: {...gpuTelemetry('vulkan+cpu'), ...patch}}).stages.portable.executed, false);
  assert.equal(resolveRuntime({fresh: false, acceleration: gpuTelemetry('vulkan+cpu')}).stages.portable.executed, false);
  const portable = enrichNativeProcessingCapabilities(available, {executable,
    driverStatus: {...driverStatus, devices: [{id: '1:2:0', name: 'FP32 GPU',
      shaderFloat64: false}]},
    // Saved resident selection must not relabel the still-running portable route.
    requested: 'gpu-resident', runtime: {...frame, acceleration: {active: 'vulkan', state: 'ready',
      device: 'gpu0', reason: '', gpuExecuted: true}}});
  assert.equal(portable.stages.portable.executed, true);
  assert.equal(portable.stages.resident.selected, false);
  assert.equal(portable.stages.resident.executed, false);
  assert.equal(portable.stages.resident.selectable, false);
  assert.equal(portable.stages.resident.reason, 'no-device-with-required-features');
  const idle = enrichNativeProcessingCapabilities(available, {executable, driverStatus});
  assert.equal(idle.stages.portable.loadable, true); // driver-status loads this module only
  assert.equal(idle.stages.resident.loadable, null);
  assert.equal(idle.stages.wideband.loadable, null);
  for (const stage of Object.values(idle.stages)) {
    assert.equal(stage.qualified, 'not-run');
    assert.equal(stage.executed, false);
  }
  for (const invalid of [{}, {fresh: false}, {nCpi: 0}, {processorGeneration: ''},
    {captureSamplePosition: {...frame.captureSamplePosition, available: false}},
    {captureSamplePosition: {...frame.captureSamplePosition, endSample: 201000}},
    {captureSamplePosition: {...frame.captureSamplePosition, epoch: Number.MAX_SAFE_INTEGER + 1}}]) {
    const runtime = Object.keys(invalid).length ? {...frame, ...invalid} : {};
    runtime.acceleration = {active: 'vulkan-resident', state: 'ready', gpuExecuted: true};
    const unbound = enrichNativeProcessingCapabilities(available, {executable, driverStatus, runtime});
    assert.equal(unbound.stages.resident.selected, true);
    assert.equal(unbound.stages.resident.executed, false);
    assert.equal(unbound.stages.resident.qualified, 'not-run');
    assert.equal(unbound.stages.resident.reason, 'frame-lineage-unavailable');
  }
  const noDriver = enrichNativeProcessingCapabilities(available, {executable, driverStatus: failedDriver});
  assert.equal(noDriver.stages.portable.discovered, null);
  assert.equal(noDriver.stages.portable.selectable, false);
  assert.equal(noDriver.stages.portable.reason, failedDriver.error);
  // Execute the production route without opening listeners. Hold each async
  // probe across a producer restart/reconnect; the response must sample the
  // post-probe generation, never the previously qualified frame.
  const server = fs.readFileSync(path.join(__dirname, 'server.js'), 'utf8');
  const routeStart = server.indexOf("app.get('/api/config/capabilities',");
  const routeEnd = server.indexOf("app.get('/api/illuminator-survey',", routeStart);
  assert(routeStart >= 0 && routeEnd > routeStart);
  for (const heldProbe of ['native', 'driver', 'localRsp']) {
    for (const transition of ['restart', 'reconnect']) {
      let release, started, handler, response;
      const pending = new Promise(resolve => { release = resolve; });
      const entered = new Promise(resolve => { started = resolve; });
      let runtime = exactRuntime('0', '201000', '241000');
      const probe = async (name, value) => {
        if (name === heldProbe) { started(); await pending; }
        return value;
      };
      vm.runInNewContext(server.slice(routeStart, routeEnd), {
        app: {get(_path, callback) { handler = callback; }},
        configFile: '/fixture/config.yml', readConfig: () => ({config: {}}),
        configuredReceiverTypes: () => [], currentGpuRuntime: () => runtime,
        readNativeProcessingCapabilities: () => probe('native', available),
        readGpuDriverStatus: () => probe('driver', driverStatus),
        localRspCurrent: () => probe('localRsp', false),
        enrichNativeProcessingCapabilities: (report, options) =>
          enrichNativeProcessingCapabilities(report, {...options, executable}),
        configFileWritable: () => true, restartCommandAvailable: () => true,
        getDeviceProfiles: () => [], FIELD_RULES: {}, RECEIVER_SYNC_HEADER: '',
        RECEIVER_SYNC_INTENT: '', receiverSyncState: {}, restartCommand: '',
        gainRestart: {installedGainRestartSupported: () => false},
        MAX_AUTOMATIC_GAIN_MS: 1, receiverAcceptanceBoundary: () => ({})
      });
      const completion = handler({}, {json(value) { response = value; }});
      await entered;
      runtime = transition === 'restart' ? {} : {...exactRuntime('18446744073709551615',
        '9007199254740993', '9007199254780993'),
        processorGeneration: 'replacement:1:1', nCpi: 1,
      };
      release();
      await completion;
      const current = response.processingBackends;
      assert.equal(current.frame.bound, transition === 'reconnect', heldProbe);
      assert.equal(current.frame.processorGeneration,
        transition === 'reconnect' ? 'replacement:1:1' : null, heldProbe);
      assert.equal(current.stages.resident.executed, transition === 'reconnect', heldProbe);
      assert.equal(current.stages.resident.qualified,
        transition === 'reconnect' ? 'ready' : 'not-run', heldProbe);
      if (transition === 'reconnect')
        assert.equal(current.frame.captureSamplePosition.epochExact, '18446744073709551615');
    }
  }
  fs.unlinkSync(path.join(directory, 'blah2-gpu-resident-vulkan.so'));
  fs.mkdirSync(path.join(directory, 'blah2-gpu-resident-vulkan.so'));
  const missing = enrichNativeProcessingCapabilities(available, {executable, driverStatus});
  assert.equal(missing.stages.resident.packaged, false);
  assert.equal(missing.stages.resident.selectable, false);
  fs.rmSync(directory, {recursive: true});
  console.log('Native processing capabilities fail closed and bind the UI to the build.');
})().catch(error => { console.error(error); process.exitCode = 1; });
