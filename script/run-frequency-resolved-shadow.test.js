'use strict';

const assert = require('assert');
const {argumentsFrom} = require('./run-frequency-resolved-shadow');

assert.deepEqual(argumentsFrom(['--config-url', 'http://127.0.0.1/api/config',
  '--fft-size', '4096']), {'config-url': 'http://127.0.0.1/api/config', 'fft-size': '4096'});
assert.throws(() => argumentsFrom(['config-url']), /Invalid argument/);
assert.throws(() => argumentsFrom(['--config-url']), /Invalid argument/);
console.log('Frequency-resolved shadow CLI argument bounds passed.');
