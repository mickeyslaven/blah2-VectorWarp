'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const yaml = require('../../api/node_modules/js-yaml');
const {corrected, points, OLD_RADIUS_M, CORRECT_RADIUS_M} =
  require('../../script/prepare-corrected-arrow-geometry');

function fixture(radiusM = OLD_RADIUS_M, note =
  '440 MHz dipoles at hole 9. Manufacturer hole radius is 7.62 inches (0.193548 m).') {
  return yaml.dump({capture: {device: {array_geometry: {
    shape: 'regular_polygon', entry_template: 'pentagon', units: 'm',
    operator_notes: note,
    elements: points(radiusM).map((position, daq_channel) =>
      ({id: `CH${daq_channel}`, daq_channel, position})),
  }}}});
}

test('converts the exact audited inch/centimetre error to hole-9 metres', () => {
  const result = corrected(fixture());
  result.positions.forEach((position, index) => position.forEach((value, axis) =>
    assert.ok(Math.abs(value - points(CORRECT_RADIUS_M)[index][axis]) < 1e-14)));
  const geometry = result.value.capture.device.array_geometry;
  assert.match(geometry.operator_notes, /7\.62 cm \(0\.0762 m\)/);
  for (const element of geometry.elements)
    assert.ok(Math.abs(Math.hypot(...element.position.slice(0, 2)) - 0.0762) < 1e-12);
});

test('preserves rotation, handedness, channel identity and source ordering', () => {
  const source = yaml.load(fixture());
  const geometry = source.capture.device.array_geometry;
  geometry.elements.forEach(element => {
    const [x, y, z] = element.position;
    element.position = [-y, x, z];
  });
  geometry.elements.reverse();
  const before = new Map(geometry.elements.map(element => [element.id,
    Math.atan2(element.position[1], element.position[0])]));
  const result = corrected(yaml.dump(source));
  assert.deepEqual(result.value.capture.device.array_geometry.elements.map(item => item.id),
    ['CH4', 'CH3', 'CH2', 'CH1', 'CH0']);
  for (const element of result.value.capture.device.array_geometry.elements) {
    assert.ok(Math.abs(Math.atan2(element.position[1], element.position[0]) -
      before.get(element.id)) < 1e-12);
    assert.ok(Math.abs(radiusForTest(element.position) - CORRECT_RADIUS_M) < 1e-12);
  }
});

const radiusForTest = position => Math.hypot(position[0], position[1]);

test('fails closed on already-different geometry', () => {
  assert.throws(() => corrected(fixture(0.1)), /exact invalidated/);
});

test('fails closed without the audited unit-error note', () => {
  assert.throws(() => corrected(fixture(OLD_RADIUS_M, 'unknown geometry')), /unit-error note/);
});

test('fails closed on duplicate IDs, duplicate channels, translation, nonplanarity, and nonfinite coordinates', () => {
  for (const mutate of [
    geometry => { geometry.elements[1].id = geometry.elements[0].id; },
    geometry => { geometry.elements[1].daq_channel = geometry.elements[0].daq_channel; },
    geometry => { geometry.elements.forEach(element => { element.position[0] += .01; }); },
    geometry => { geometry.elements[2].position[2] = .01; },
    geometry => { geometry.elements[2].position[0] = Number.NaN; },
  ]) {
    const value = yaml.load(fixture());
    mutate(value.capture.device.array_geometry);
    assert.throws(() => corrected(yaml.dump(value)), /refusing/);
  }
});
