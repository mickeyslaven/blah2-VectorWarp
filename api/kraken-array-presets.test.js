'use strict';

const assert = require('assert');
const geometryUi = require('../html/js/kraken_geometry');
const {evaluateKrakenArray} = require('./kraken-array');
const {validateConfig} = require('./config-manager');
const {getSettingsMapping} = require('./receiver-manager');

const base = require('./config-store').configurationForReceiver('Kraken');
const clone = value => JSON.parse(JSON.stringify(value));
const C = 299792458;

for (const depth of [2, 4, 8]) {
  const temporal = clone(base);
  temporal.process.detection.integration_cpis = 2;
  temporal.process.detection.integration_motion_bins = 1;
  Object.assign(temporal.process.detection.track_before_detect,
    {enable: depth > 2, max_cpis: Math.max(4, depth), fractional_delay: depth > 2});
  temporal.process.tracker.initiate.N = 1 + depth * (temporal.process.tracker.initiate.M - 1);
  temporal.process.tracker.delete = Math.max(temporal.process.tracker.delete, depth - 1);
  const requested = clone(temporal.process.detection);
  for (const change of [
    config => geometryUi.applyPreset(config, 'cross4ref'),
    config => geometryUi.applyPreset(config, 'ula5'),
    config => geometryUi.applyChannelRole(config, 4, 'dedicated_reference'),
    config => geometryUi.applyReferenceMode(config, 'dedicated')
  ]) {
    const config = clone(temporal);
    geometryUi.applyPreset(config, 'pentagon5');
    config.process.tracker.beam_tolerance = 1;
    change(config);
    assert.deepEqual(config.process.detection, requested,
      `${depth}-CPI detector must survive every topology-edit helper call site`);
    assert.equal(config.process.tracker.beam_tolerance, undefined,
      'incompatible beam gate is removed explicitly with coherent array features');
    assert.deepEqual(config.process.tracker, temporal.process.tracker,
      'reference topology preserves canonical confirmation/recovery policy');
    assert.deepEqual(config.process.performance, temporal.process.performance,
      'reference topology preserves the selected backend policy');
    assert.equal(validateConfig(config).valid, true, validateConfig(config).errors.join('; '));
  }
}

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

const roles = clone(base);
roles.capture.device.channel_count = 5;
geometryUi.applyPreset(roles, 'pentagon5');
geometryUi.applyChannelRole(roles, 4, 'dedicated_reference');
assert.equal(geometryUi.channelRole(roles, 4), 'dedicated_reference');
assert.equal(roles.process.reference_synthesis.mode, 'dedicated');
assert.deepEqual(roles.process.reference_synthesis.channels, [4]);
assert.ok(!roles.capture.device.surveillance_channels.includes(4));
assert.ok(!roles.capture.device.array_geometry.bearing_channels.includes(4));
geometryUi.applyReferenceMode(roles, 'array_eigenbeam');
assert.equal(geometryUi.channelRole(roles, 4), 'array');
assert.ok(roles.capture.device.surveillance_channels.includes(4));
assert.ok(roles.capture.device.array_geometry.bearing_channels.includes(4));
assert.equal(validateConfig(roles).valid, true, validateConfig(roles).errors.join('; '));

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
