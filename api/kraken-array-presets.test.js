'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const geometryUi = require('../html/js/kraken_geometry');
const {evaluateKrakenArray} = require('./kraken-array');
const {validateConfig} = require('./config-manager');
const {getSettingsMapping} = require('./receiver-manager');

const base = yaml.load(fs.readFileSync(
  path.join(__dirname, '..', 'config', 'config-kraken.yml'), 'utf8'));
const clone = value => JSON.parse(JSON.stringify(value));
const C = 299792458;

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
  const bearingPoints = array.elements.filter(item =>
    array.bearing_channels.includes(item.daq_channel)).map(item => item.position);
  const adjacent = bearingPoints.map((point, index) => {
    const other = preset.shape === 'ula' ? bearingPoints[index + 1] :
      bearingPoints[(index + 1) % bearingPoints.length];
    return other ? Math.hypot(point[0] - other[0], point[1] - other[1], point[2] - other[2]) : null;
  }).filter(value => value !== null);
  const halfWavelength = C / config.capture.fc / 2;
  adjacent.forEach(value => assert.ok(Math.abs(value - halfWavelength) < 1e-10,
    `${preset.id} adjacent bearing elements use exact wavelength/2 spacing`));
  const centre = [0, 1, 2].map(axis => bearingPoints.reduce((sum, point) => sum + point[axis], 0) / bearingPoints.length);
  centre.forEach(value => assert.ok(Math.abs(value) < 1e-10,
    `${preset.id} bearing aperture remains centred at the array origin`));
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
assert.equal(geometryUi.applyPreset(mismatch, 'octagon8'), false,
  'an eight-channel frontend layout cannot mutate a five-channel receiver');
assert.equal(JSON.stringify(mismatch), before,
  'rejected frontend layouts leave the backend draft untouched');

const receiverBoundary = new Map(getSettingsMapping('Kraken')
  .map(item => [item.configField, item]));
assert.equal(receiverBoundary.get('capture.device.channel_count').controlOperation,
  'set_num_elements', 'active channel count is applied to Suite and read back');
for (const field of ['capture.device.reference_channel',
  'capture.device.surveillance_channels', 'process.reference_synthesis'])
  assert.equal(receiverBoundary.get(field).direction, 'config-only',
    `${field} is a VectorWarp DSP role, not an invented DAQ command`);
assert.equal(receiverBoundary.has('capture.device.array_geometry'), false,
  'physical coordinates remain in VectorWarp because the DAQ has no geometry contract');

console.log('Frontend array presets pass managed and runtime 2-8 channel backend contracts.');
