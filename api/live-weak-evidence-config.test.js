const assert = require('assert');
const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const {validateConfig} = require('./config-manager.js');

const profile = yaml.load(fs.readFileSync(
  path.join(__dirname, '..', 'config', 'config-kraken.yml'), 'utf8'));
const configured = structuredClone(profile);
Object.assign(configured.process.detection.live_temporal_shadow, {
  enabled: true, artifact_only: true,
  artifact_path: '/var/tmp/unique-hp-weak-evidence.vww',
  artifact_max_mb: 512, artifact_max_frames: 8,
  max_work_ms: 100, min_interval_ms: 400,
  delay_motion_bins: 0, doppler_motion_bins: 0,
});
assert.equal(profile.process.detection.live_temporal_shadow.enabled, false);
assert.equal(validateConfig(configured).valid, true);
assert.equal(configured.process.detection.pfa, profile.process.detection.pfa);
assert.deepStrictEqual(configured.process.tracker, profile.process.tracker);

for (const [key, value] of [
  ['artifact_path', 'relative.vww'], ['artifact_path', '/tmp/invalid\0.vww'],
  ['artifact_path', '/' + 'a'.repeat(4096)], ['artifact_path', ''],
  ['artifact_path', null], ['artifact_only', 'true'],
  ['artifact_max_mb', 0], ['artifact_max_mb', 1025], ['artifact_max_mb', 1.5],
  ['artifact_max_mb', '512'], ['artifact_max_frames', 0],
  ['artifact_max_frames', 129], ['artifact_max_frames', 1.5],
  ['enabled', false],
]) {
  const invalid = structuredClone(configured);
  invalid.process.detection.live_temporal_shadow[key] = value;
  assert.equal(validateConfig(invalid).valid, false, `${key}=${JSON.stringify(value)}`);
}
for (const maximum of [1, 1024]) {
  const bounded = structuredClone(configured);
  bounded.process.detection.live_temporal_shadow.artifact_max_mb = maximum;
  assert.equal(validateConfig(bounded).valid, true);
}
for (const maximum of [1, 128]) {
  const bounded = structuredClone(configured);
  bounded.process.detection.live_temporal_shadow.artifact_max_frames = maximum;
  assert.equal(validateConfig(bounded).valid, true);
}
console.log('Live weak-evidence configuration: defaults, bounds, identity and decision parity passed.');
