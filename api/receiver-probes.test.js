'use strict';

const assert = require('assert/strict');
const {createReceiverProbes, dependenciesFromCache, serviceFromProperties,
  receiverStatusFromJson} = require('./receiver-probes');

async function main() {
  const libraries = dependenciesFromCache([
    ' libhackrf.so.0 (libc6,x86-64) => /lib/libhackrf.so.0',
    ' libsdrplay_api.so.3.15 (libc6,x86-64) => /lib/libsdrplay_api.so.3.15',
    ' libuhd.so.4.8.0 (libc6,x86-64) => /lib/libuhd.so.4.8.0'
  ].join('\n'));
  assert.equal(libraries.HackRF.state, 'installed');
  assert.equal(libraries.RspDuo.version, '3.15');
  assert.equal(libraries.Usrp.version, '4.8.0');
  assert.equal(libraries.Kraken.state, 'unknown');
  assert.equal(dependenciesFromCache(' libuhd.so.4.6.0 => /lib/libuhd.so.4.6.0').Usrp.state, 'unknown');
  assert.equal(dependenciesFromCache('').HackRF.state, 'unknown');
  assert.throws(() => dependenciesFromCache('x'.repeat(262145)), /inventory/);
  assert.deepEqual(serviceFromProperties('LoadState=loaded\nActiveState=active\n'), {state: 'running'});
  assert.deepEqual(serviceFromProperties('LoadState=loaded\nActiveState=failed\n'), {state: 'stopped'});
  assert.deepEqual(serviceFromProperties('LoadState=not-found\nActiveState=inactive\n'), {state: 'unknown'});
  const status = receiverStatusFromJson(JSON.stringify({schema: 1, hardwareProbed: false,
    receivers: ['Kraken', 'RspDuo', 'Usrp', 'HackRF'].map((receiver, index) => ({receiver,
      builtIn: receiver === 'Kraken', compiled: true, moduleLoadable: index !== 1,
      error: index === 1 ? 'Runtime dependency unavailable.' : ''}))}));
  assert.equal(status.Kraken.builtIn, true);
  assert.equal(status.RspDuo.moduleLoadable, false);
  assert.throws(() => receiverStatusFromJson(JSON.stringify({schema: 1, hardwareProbed: true,
    receivers: []})), /capability/);

  const touched = [];
  const config = {capture: {device: {type: 'Kraken', heimdall: {host: '127.0.0.1', port: 8091}}}};
  let upstreamCalls = 0;
  const probes = createReceiverProbes(config, {
    readdir: async () => ['1-1', '1-1:1.0', '../../etc', '.', '..'],
    readFile: async file => {
      touched.push(file);
      return file.endsWith('/product') ? 'HackRF One' : file.endsWith('/manufacturer') ? 'Great Scott Gadgets' : 'serial';
    },
    exists: async () => true,
    run: async (file, args) => {
      assert.equal(file, '/usr/bin/systemctl');
      assert.deepEqual(args.slice(0, 5), ['show', '--no-pager', '--property=LoadState', '--property=ActiveState', '--']);
      assert.ok(args.slice(5).every(unit => unit.endsWith('.service')));
      return 'LoadState=loaded\nActiveState=inactive\n';
    },
    upstream: async () => { upstreamCalls++; return {available: true, matched: true}; },
    receiverStatus: async () => status
  });
  assert.equal((await probes.usbInventory({})).length, 1);
  assert.equal(touched.length, 3);
  assert.ok(touched.every(file => file.startsWith('/sys/bus/usb/devices/1-1/')));
  assert.deepEqual(await probes.serviceStatus({serviceId: 'kraken-suite-v2'}), {state: 'stopped'});
  assert.deepEqual(await probes.serviceStatus({serviceId: 'sdrplay-api'}), {state: 'stopped'});
  assert.deepEqual(await probes.nativeReceiverStatus({}), status);
  await assert.rejects(probes.serviceStatus({serviceId: 'arbitrary.service'}), /Unknown/);
  assert.equal((await probes.configuredUpstreamStatus({host: '127.0.0.1', dataPort: 8091, controlPort: 8092})).available, true);
  await assert.rejects(probes.configuredUpstreamStatus({host: 'other-host', dataPort: 8091, controlPort: 8092}), /endpoint changed/);
  assert.equal(upstreamCalls, 1);
  config.capture.device.heimdall.host = 'changed-later';
  assert.equal((await probes.configuredUpstreamStatus({host: '127.0.0.1', dataPort: 8091, controlPort: 8092})).available, true);
  assert.equal(upstreamCalls, 2);
  const controller = new AbortController(); controller.abort();
  await assert.rejects(probes.usbInventory({}, {signal: controller.signal}), /cancelled/);
  const oversized = createReceiverProbes(config, {readdir: async () => Array(513).fill('1-1')});
  await assert.rejects(oversized.usbInventory({}), /limit/);
  console.log('Read-only receiver probe tests passed; no host, service or hardware was accessed.');
}

main().catch(error => { console.error(error); process.exitCode = 1; });
