#!/usr/bin/env node
'use strict';

// Emit a new configuration for review. Existing operator files are never read
// or written; Settings uses the same receiver-profile generator.
const {configurationForReceiver, setupDefaults} = require('./config-store');
const {getDeviceProfiles} = require('./config-manager');
const yaml = require('js-yaml');

const type = process.argv[2];
if (process.argv.length !== 3 ||
    !['Unconfigured', ...getDeviceProfiles().map(profile => profile.type)].includes(type)) {
  console.error('Usage: node api/config-generate.js Unconfigured|Kraken|Usrp|HackRF|RspDuo');
  process.exitCode = 2;
} else {
  process.stdout.write(yaml.dump(type === 'Unconfigured' ? setupDefaults() :
    configurationForReceiver(type), {lineWidth: 100}));
}
