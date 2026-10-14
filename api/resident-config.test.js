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
