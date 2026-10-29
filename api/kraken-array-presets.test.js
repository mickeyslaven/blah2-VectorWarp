'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const geometryUi = require('../html/js/kraken_geometry');
const {evaluateKrakenArray} = require('./kraken-array');
const {validateConfig} = require('./config-manager');

const base = yaml.load(fs.readFileSync(
  path.join(__dirname, '..', 'config', 'config-kraken.yml'), 'utf8'));
const clone = value => JSON.parse(JSON.stringify(value));

for (const preset of geometryUi.layoutPresets()) {
  const config = clone(base);
  config.capture.device.channel_count = preset.channels;
  assert.equal(geometryUi.applyPreset(config, preset.id), true,
    `${preset.id} must materialize a real configuration`);
  const device = config.capture.device;
  const synthesis = config.process.reference_synthesis;
  const array = device.array_geometry;
  assert.equal(array.elements.length, preset.channels,
    `${preset.id} maps every active backend channel exactly once`);
  assert.deepEqual(array.bearing_channels,
    Array.from({length: preset.bearing}, (_, index) => index));
  assert.equal(new Set(array.elements.map(item => item.daq_channel)).size,
    preset.channels, `${preset.id} has a one-to-one DAQ mapping`);
  if (preset.dedicated) {
    const reference = preset.channels - 1;
    assert.equal(device.reference_channel, reference);
    assert.deepEqual(synthesis.channels, [reference]);
    assert.ok(!device.surveillance_channels.includes(reference));
    assert.ok(!array.bearing_channels.includes(reference));
  } else {
    assert.equal(synthesis.mode, 'array_eigenbeam');
    assert.deepEqual(synthesis.channels, array.bearing_channels);
    assert.deepEqual(device.surveillance_channels, array.bearing_channels);
  }
  const backend = evaluateKrakenArray(config, {num_channels: preset.channels});
  assert.equal(backend.valid, true,
    `${preset.id} must pass the runtime array contract: ${backend.issues.map(i => i.code).join(', ')}`);
  const managed = validateConfig(config);
  assert.equal(managed.valid, true,
    `${preset.id} must pass managed configuration validation: ${managed.errors.join('; ')}`);
}

const mismatch = clone(base);
mismatch.capture.device.channel_count = 5;
const before = JSON.stringify(mismatch);
assert.equal(geometryUi.applyPreset(mismatch, 'ring8'), false,
  'an eight-channel frontend layout cannot mutate a five-channel receiver');
assert.equal(JSON.stringify(mismatch), before,
  'rejected frontend layouts leave the backend draft untouched');

console.log('Frontend array presets pass managed and runtime 2-8 channel backend contracts.');
