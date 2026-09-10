'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {getDeviceProfiles, validateConfig, writeConfigAtomically} = require('./config-manager');
const {readConfig, saveConfig, setupDefaults} = require('./config-store');
const {evaluateKrakenArray} = require('./kraken-array');
const geometry = require('../html/js/kraken_geometry');
const {applyDeviceProfile, normalizeKrakenChannels} = require('../html/js/config_ui');
const clone = value => JSON.parse(JSON.stringify(value));
function fixture(count = 5, reference = 2) {
  const config = applyDeviceProfile(setupDefaults(), getDeviceProfiles()[0]);
  const device = config.capture.device;
  device.channel_count = count; device.reference_channel = reference;
  device.surveillance_channels = Array.from({length: count}, (_, i) => i).filter(i => i !== reference);
  config.process.reference_synthesis.mode = 'dedicated';
  config.process.reference_synthesis.channels = [reference];
  device.array_geometry = geometry.emptyRecord(count);
  device.array_geometry.elements.forEach((element, index) => { element.daq_channel = index; element.position = [index * .1, index % 2 * .2, 0]; });
  device.array_geometry.bearing_channels = [...device.surveillance_channels];
  return config;
}
const base = fixture();
for (const shape of ['cross', 'regular_polygon', 'ula', 'custom']) {
  const config = fixture();
  const record = config.capture.device.array_geometry;
  record.shape = shape;
  record.elements[config.capture.device.reference_channel].position = [2, 2, .2];
  const selected = record.bearing_channels;
  selected.forEach((channel, index) => {
    record.elements[channel].position = shape === 'ula' ? [index * .1, 0, 0] :
      shape === 'custom' ? [index * .1, index % 2 * .2, index * .05] :
      [.5 * Math.cos(index * Math.PI / 2), .5 * Math.sin(index * Math.PI / 2), 0];
  });
  assert.equal(validateConfig(config).valid, true, `${shape} persists`);
  assert.equal(validateConfig(config).arrayGeometry.valid, true, `${shape} is a complete record`);
  assert.equal(validateConfig(config).arrayGeometry.worldBearingEligible, false);
}
for (let count = 2; count <= 8; count++) {
  for (let reference = 0; reference < count; reference++) {
    const config = fixture(count, reference);
    assert.equal(validateConfig(config).valid, true, `${count} inputs, reference ${reference}`);
    assert.equal(evaluateKrakenArray(config).bearingImplemented, false);
    delete config.capture.device.array_geometry;
    assert.equal(validateConfig(config).valid, true, 'Legacy geometry omission');
    config.capture.device.array_geometry = geometry.emptyRecord(count);
    assert.equal(validateConfig(config).valid, true, 'Incomplete metadata must allow passive radar');
  }
}
for (const mutate of [
  g => { g.units = 'mm'; }, g => { g.shape = 'sphere'; }, g => { g.coordinate_frame = 'left-handed'; },
  g => { g.elements[0].position = [0, 0]; }, g => { g.elements[0].position = ['1', 0, 0]; },
  g => { g.elements[0].position = [Infinity, 0, 0]; }, g => { g.elements[0].position = [NaN, 0, 0]; },
  g => { g.elements[0].position = [Number.MAX_SAFE_INTEGER + 1, 0, 0]; },
  g => { g.elements[0].daq_channel = 8; }, g => { g.elements[0].daq_channel = '0'; },
  g => { g.elements[1].daq_channel = 0; }, g => { g.elements[1].id = g.elements[0].id; },
  g => { g.elements[0].receiver_serial = 1000; }, g => { g.elements[0].receiver_serial = 'x\n'; },
  g => { g.elements[0].receiver_serial = 'x'.repeat(129); }, g => { g.mapping_confirmed = 'true'; },
  g => { g.x_axis_bearing_deg_true = 360; }, g => { g.x_axis_bearing_deg_true = -1; },
  g => { g.bearing_channels = [0, 0]; }, g => { g.bearing_channels = Array(2); },
  g => { g.elements = Array(9); }, g => { g.extra = {constructor: {polluted: true}}; },
  g => { g.extra = {a: {b: {c: {d: {e: {f: {g: 1}}}}}}}; }
]) {
  const candidate = fixture(); mutate(candidate.capture.device.array_geometry);
  assert.equal(validateConfig(candidate).valid, false, String(mutate));
}
assert.deepEqual(geometry.recordErrors(geometry.emptyRecord(5)), []);
assert.ok(geometry.recordErrors(null).length);
assert.ok(geometry.recordErrors('wrong').length);
assert.ok(geometry.recordErrors({...geometry.emptyRecord(2), elements: Array(2)}).length);
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'kraken-geometry-config-'));
const file = path.join(directory, 'config.yml');
try {
  base.capture.device.array_geometry.site_note = {operator: 'fixture only', tags: ['bench', 'unverified']};
  base.capture.device.array_geometry.elements[0].measurement_note = 'cable A';
  writeConfigAtomically(file, base);
  let saved = readConfig(file);
  assert.equal(saved.validation.valid, true);
  assert.deepEqual(saved.config.capture.device.array_geometry, base.capture.device.array_geometry);
  const attested = clone(saved.config);
  Object.assign(attested.capture.device.array_geometry, {mapping_confirmed: true, geometry_confirmed: true,
    orientation_confirmed: true, x_axis_bearing_deg_true: 30});
  saved = saveConfig(file, attested, saved.revision);
  assert.equal(saved.config.capture.device.array_geometry.mapping_confirmed, false, 'New orientation clears previous statements');
  Object.assign(saved.config.capture.device.array_geometry, {mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true});
  saved = saveConfig(file, saved.config, saved.revision);
  assert.equal(saved.config.capture.device.array_geometry.mapping_confirmed, true, 'Unchanged measurements can carry operator statements');
  const confirmed = clone(saved.config);
  for (const mutate of [
    c => { c.capture.device.channel_count = 4; }, c => { c.capture.device.reference_channel = 4; },
    c => { c.capture.device.surveillance_channels.reverse(); }, c => { c.process.reference_synthesis.mode = 'array_eigenbeam'; },
    c => { c.capture.fc += 100; }, c => { c.capture.fs += 1; }, c => { c.capture.device.heimdall.host = '192.0.2.1'; },
    c => { c.capture.device.array_geometry.elements.reverse(); },
    c => { c.capture.device.array_geometry.elements[0].position[0] += 1; },
    c => { c.capture.device.array_geometry.elements[0].receiver_serial = 'operator-entry'; },
    c => { c.capture.device.array_geometry.bearing_channels.reverse(); },
    c => { c.capture.device.array_geometry.x_axis_bearing_deg_true = 45; }
  ]) {
    const candidate = clone(confirmed); mutate(candidate);
    geometry.reconcile(candidate, confirmed);
    for (const field of ['mapping_confirmed', 'geometry_confirmed', 'orientation_confirmed']) assert.equal(candidate.capture.device.array_geometry[field], false);
  }
  const resized = clone(confirmed); resized.capture.device.channel_count = 2; normalizeKrakenChannels(resized, 5);
  assert.deepEqual(resized.capture.device.array_geometry.elements, confirmed.capture.device.array_geometry.elements);
  assert.equal(validateConfig(resized).valid, true, 'Inactive measurements stay as review metadata');
  resized.capture.device.channel_count = 5; normalizeKrakenChannels(resized, 2);
  assert.equal(resized.capture.device.array_geometry.mapping_confirmed, false);
  for (const profile of getDeviceProfiles()) {
    const candidate = applyDeviceProfile(clone(confirmed), profile);
    if (profile.type === 'HackRF') candidate.capture.device.serial = ['001', '002'];
    assert.equal(validateConfig(candidate).valid, true, profile.type);
    assert.deepEqual(candidate.capture.device.array_geometry.elements, confirmed.capture.device.array_geometry.elements);
    assert.equal(candidate.capture.device.array_geometry.mapping_confirmed, false);
  }
  const changed = clone(confirmed); changed.capture.device.array_geometry.elements[0].position[0] = .125;
  saved = saveConfig(file, changed, saved.revision);
  assert.equal(saved.config.capture.device.array_geometry.elements[0].position[0], .125);
  assert.equal(saved.config.capture.device.array_geometry.mapping_confirmed, false);
  assert.equal(saved.config.capture.device.array_geometry.elements[0].measurement_note, 'cable A');
  assert.throws(() => saveConfig(file, changed, 'stale'), /changed/);
  const invalid = clone(changed); invalid.capture.device.array_geometry.units = 'mm';
  assert.throws(() => saveConfig(file, invalid, saved.revision), /metres/);
  for (const status of [null, {num_channels: 5}, {available: false, num_channels: 5},
    {available: true, checkedAt: 0, num_channels: 5}, {num_channels: 5, actual: {channels: 4}},
    {num_channels: 4}, {receiver_serials: ['inferred'], array_geometry: {verified: true}}]) {
    const result = evaluateKrakenArray(confirmed, status);
    for (const flag of ['bearingImplemented', 'relativeBearingEligible', 'worldBearingEligible']) assert.equal(result[flag], false);
    assert.equal(result.verification.physicalAgreement.state, 'unverified');
    assert.equal(result.verification.statusFreshness.state, 'unverified');
  }
  console.log('Kraken metadata schema, 2–8 roles, persistence, profile retention and attestation invalidation passed.');
} finally { fs.rmSync(directory, {recursive: true, force: true}); }
