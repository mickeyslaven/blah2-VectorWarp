'use strict';

const assert = require('assert/strict');
const {receiverSetupGuide} = require('./receiver-setup-guide');

const base = {
  type: 'RspDuo', locality: 'local',
  capabilities: {liveCompiled: true},
  managedService: {state: 'unknown'},
  dependencies: {state: 'unknown'}
};

const unknown = receiverSetupGuide(base, '/opt/vectorwarp/libexec/vectorwarp-receiver-helper');
assert.equal(unknown[0].link, 'https://sdrplay.com/hardware-api/');
assert.match(unknown[0].text, /Could not verify/);
assert.ok(!unknown.some(step => step.command?.includes('enroll-service RspDuo')),
  'Unknown SDK inventory must not authorize enrollment.');

const stopped = receiverSetupGuide({...base, dependencies: {state: 'installed'},
  managedService: {state: 'stopped'}}, '/opt/vectorwarp/libexec/vectorwarp-receiver-helper');
assert.ok(stopped.some(step => step.command?.endsWith('enroll-service RspDuo')),
  'Only an observed installed SDK may receive the existing enrollment guidance.');

const running = receiverSetupGuide({...base, dependencies: {state: 'installed'},
  managedService: {state: 'running'}}, '/opt/vectorwarp/libexec/vectorwarp-receiver-helper');
assert.ok(!running.some(step => step.command?.endsWith('enroll-service RspDuo')));
console.log('SDRplay setup guidance tests passed.');
