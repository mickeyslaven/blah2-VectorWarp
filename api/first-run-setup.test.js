'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const {validateConfig, getDeviceProfiles} = require('./config-manager');
const {readConfig, configurationForReceiver} = require('./config-store');

const filename = path.join(__dirname, '..', 'config', 'config.yml');
const raw = yaml.load(fs.readFileSync(filename, 'utf8'));
const document = readConfig(filename);

assert.equal(raw.capture.device.type, 'Unconfigured');
assert.equal(document.setupRequired, true);
assert.equal(document.validation.valid, false);
assert.ok(document.validation.errors.includes('capture.device.type is not supported'));

for (const profile of getDeviceProfiles()) {
  const selected = configurationForReceiver(profile.type, document.config);
  if (profile.type === 'HackRF') selected.capture.device.serial = ['0001', '0002'];
  const validation = validateConfig(selected);
  assert.equal(validation.valid, true,
    `${profile.type}: ${validation.errors.join('; ')}`);
  assert.equal(selected.capture.device.type, profile.type);
}

console.log('Canonical first-run config is neutral; every explicit receiver selection validates.');
