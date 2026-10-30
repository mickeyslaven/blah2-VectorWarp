'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const root = path.resolve(process.argv[2]);
const fromApi = createRequire(path.join(root, 'api', 'config-manager.js'));
const {validateConfig, FIELD_RULES} = fromApi('./config-manager.js');
const yaml = fromApi('js-yaml');
const load = () => yaml.load(fs.readFileSync(path.join(root, 'config', 'config-kraken.yml'), 'utf8'));
function valid(config) {
  const result = validateConfig(config);
  assert.equal(result.valid, true, result.errors.join('; '));
}
function invalid(config, fragment) {
  const result = validateConfig(config);
  assert.equal(result.valid, false);
  assert.ok(result.errors.some(value => value.includes(fragment)), result.errors.join('; '));
}
for (const mode of ['auto', 'cpu', 'gpu']) {
  const c = load(); c.process.performance.acceleration = mode;
  delete c.process.tracker.gpu_capacity; valid(c);
  assert.equal(c.process.tracker.gpu_capacity, undefined);
}
function resident() {
  const c = load(); c.process.performance.acceleration = 'gpu-resident';
  c.process.tracker.gpu_capacity = 128; return c;
}
const c = resident();const before = JSON.stringify(c);valid(c);
assert.equal(JSON.stringify(c), before, 'Validation rewrote the selected workload or capacity');
const integrated = resident();integrated.process.detection.integration_cpis = 2;valid(integrated);
const tbd = resident();
tbd.process.surveillance_fusion.mode = 'coherent_beam_bank';
tbd.capture.device.array_geometry = {
  shape: 'regular_polygon', units: 'm', coordinate_frame: 'array_xy_right_handed',
  mapping_confirmed: false, geometry_confirmed: false, orientation_confirmed: false,
  x_axis_bearing_deg_true: 0,
  bearing_channels: [...tbd.capture.device.surveillance_channels],
  elements: tbd.capture.device.surveillance_channels.map((daq_channel, index) => ({
    id: `channel-${daq_channel}`, daq_channel, receiver_serial: String(1000 + daq_channel),
    position: [Math.cos(2 * Math.PI * index / 5) * .2,
      Math.sin(2 * Math.PI * index / 5) * .2, 0]
  }))
};
tbd.process.detection.track_before_detect = {enable: true, max_cpis: 8, motion_bins: 1};
valid(tbd);
for (const mutate of [
  candidate => { candidate.capture.device.array_geometry.elements[0].position[0] += .001; },
  candidate => { candidate.capture.device.array_geometry.elements[0].position[2] += .001; },
  candidate => { [candidate.capture.device.array_geometry.elements[1].position,
    candidate.capture.device.array_geometry.elements[2].position] =
    [candidate.capture.device.array_geometry.elements[2].position,
      candidate.capture.device.array_geometry.elements[1].position]; }
]) {
  const candidate = structuredClone(tbd); mutate(candidate);
  invalid(candidate, 'exact planar uniformly stepped regular polygon');
}
const starOrdered = structuredClone(tbd);
const starOrder = [0, 2, 4, 1, 3];
starOrdered.capture.device.surveillance_channels = starOrder.map(index =>
  tbd.capture.device.surveillance_channels[index]);
starOrdered.process.reference_synthesis.channels =
  [...starOrdered.capture.device.surveillance_channels];
valid(starOrdered);
for (const value of [3, 5, 6, 7, 9]) {
  const candidate = structuredClone(tbd);
  candidate.process.detection.track_before_detect.max_cpis = value;
  invalid(candidate, 'max_cpis');
}
const noCoherentTbd = resident();
noCoherentTbd.process.detection.track_before_detect.enable = true;
invalid(noCoherentTbd, 'coherent surveillance fusion');
for (const mode of ['auto', 'cpu', 'gpu']) {
  const candidate = load();candidate.process.performance.acceleration = mode;
  candidate.process.detection.integration_cpis = 2;
  invalid(candidate, 'multi-CPI detection requires gpu-resident acceleration');
}
const missing = resident();delete missing.process.tracker.gpu_capacity;
invalid(missing, 'process.tracker.gpu_capacity');
for (const value of [0, -1, 1.5, 4294967296, '128', NaN, Infinity]) {
  const c = resident();c.process.tracker.gpu_capacity = value;
  invalid(c, 'process.tracker.gpu_capacity');
}
const noTracker = resident();noTracker.process.tracker.enable = false;
delete noTracker.process.tracker.gpu_capacity;valid(noTracker);
const noClutter = resident();noClutter.process.clutter.enable = false;invalid(noClutter, 'requires clutter');
const noDetection = resident();noDetection.process.detection.enable = false;invalid(noDetection, 'requires process.detection.enable');
assert.ok(FIELD_RULES['process.performance.acceleration'].choices.includes('gpu-resident'));
assert.equal(FIELD_RULES['process.tracker.gpu_capacity'].optional, true);
console.log('COMPLETE resident-config-api validation only; no receiver, GPU, network, or config mutation');
