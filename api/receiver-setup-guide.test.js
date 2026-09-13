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
const usrp = receiverSetupGuide({...base, type: 'Usrp',
  dependencies: {state: 'missing'}}, '/opt/vectorwarp/libexec/vectorwarp-receiver-helper');
const uhdSteps = usrp.filter(step => /UHD/.test(step.text));
assert.equal(uhdSteps.length, 2);
uhdSteps.forEach(step => assert.match(step.text, /UHD 4\.1 or newer/));
assert.ok(!usrp.some(step => /4\.8/.test(step.text)));
console.log('SDRplay startup and UHD minimum-version guidance tests passed.');
