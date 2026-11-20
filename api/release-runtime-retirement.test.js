'use strict';

// Release regression: retired experiment settings cannot recreate live lanes,
// and upgrades preserve every unrelated operator setting and the old bytes.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const yaml = require('js-yaml');
const {FIELD_RULES, validateConfig} = require('./config-manager');
const {configurationForReceiver, stripLegacyNoOps, migrateKnownDefaults,
  readConfig} = require('./config-store');

const retired = ['temporal_shadow', 'live_temporal_shadow', 'track_guided_shadow',
  'frequency_resolved_shadow', 'doppler_ridge_scrubber', 'manifold_automation',
  'detection_manifold_automation'];
for (const receiver of ['Kraken', 'Usrp', 'RspDuo', 'HackRF']) {
  const config = configurationForReceiver(receiver);
  if (receiver === 'HackRF') config.capture.device.serial = ['0001', '0002'];
  for (const name of retired) assert.ok(!JSON.stringify(config).includes(name), receiver + ': ' + name);
  for (const depth of [2, 4, 8]) {
    const candidate = structuredClone(config);
    candidate.process.detection.integration_cpis = depth === 2 ? 2 : 1;
    candidate.process.detection.track_before_detect.enable = depth > 2;
    candidate.process.detection.track_before_detect.max_cpis = depth > 2 ? depth : 8;
    candidate.process.tracker.initiate.N = 1 + depth * (candidate.process.tracker.initiate.M - 1);
    candidate.process.tracker.delete = Math.max(candidate.process.tracker.delete, depth - 1);
    assert.ok(validateConfig(candidate).valid, receiver + ': production temporal depth ' + depth);
  }
}
for (const name of retired)
  assert.ok(!Object.keys(FIELD_RULES).some(key => key.includes(name)), name);

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-retired-runtime-'));
try {
  for (const scored of [false, true]) {
    const config = configurationForReceiver('Kraken');
    config.process.detection.live_temporal_shadow = {enabled: true, artifact_only: !scored,
      score_tracks: scored, association_sustain: scored, artifact_path: '/private/retired.vww'};
    config.process.detection.temporal_shadow = {enabled: false, artifact_path: '/private/replay.vwtbd'};
    config.process.detection.doppler_ridge_scrubber = {mode: scored ? 'shadow' : 'active',
      shadow_maximum_cpis: 512};
    config.process.tracker.track_guided_shadow = {enable: true, promote: false, sustain: false};
    config.process.bearing.frequency_resolved_shadow = {enabled: true, fft_size: 2048};
    config.process.bearing.manifold_automation = {enabled: true, auto_promote: false};
    config.process.surveillance_fusion.detection_manifold_automation = {
      enabled: true, auto_promote: false};
    config.operator_metadata = {retained: 'site-specific', values: [0, false, '']};
    const expected = stripLegacyNoOps(structuredClone(config));
    const filename = path.join(directory, scored ? 'scored-live.yml' : 'artifact-live.yml');
    const before = yaml.dump(config);
    fs.writeFileSync(filename, before);
    assert.equal(migrateKnownDefaults(filename), true);
    assert.deepEqual(yaml.load(fs.readFileSync(filename, 'utf8')), expected);
    assert.equal(fs.readFileSync(filename + '.bak', 'utf8'), before);
    assert.equal(migrateKnownDefaults(filename), false, 'migration must be idempotent');
    const loaded = readConfig(filename);
    for (const name of retired) assert.ok(!JSON.stringify(loaded.config).includes(name));
    assert.deepEqual(loaded.config.operator_metadata, config.operator_metadata);
  }
} finally { fs.rmSync(directory, {recursive: true, force: true}); }

const root = path.join(__dirname, '..');
for (const filename of ['src/blah2.cpp', 'src/process/pipeline/CanonicalRadar.h',
  'src/process/pipeline/CanonicalRadar.cpp', 'src/process/pipeline/RadarPipeline.h',
  'api/server.js', 'html/js/config_ui.js', 'html/js/config_profiles.js',
  'html/js/processing_capabilities.js', 'config/config.yml']) {
  const content = fs.readFileSync(path.join(root, filename), 'utf8');
  assert.equal(/live_temporal_shadow|temporal_shadow|track_guided_shadow|frequency_resolved_shadow|doppler_ridge_scrubber|manifold_automation|detection_manifold_automation|ManifoldAutomation|LiveTemporalShadow|TemporalShadowTracker|DopplerRidgeScrubber|comparison_/.test(content), false,
    filename + ': release entrypoints must not compile, start or expose experiment lanes');
}
for (const filename of ['src/blah2.cpp', 'src/process/pipeline/CanonicalRadar.h',
  'src/process/pipeline/CanonicalRadar.cpp', 'src/process/pipeline/RadarPipeline.h',
  'src/process/integration/DirectPathCoherentReplay.h',
  'src/process/integration/DirectPathCoherentReplay.cpp']) {
  const content = fs.readFileSync(path.join(root, filename), 'utf8');
  assert.equal(/DirectPathCoherentExperiment|DirectPathCoherentReplayReader|PostClutterBootstrap|CoherentPathEvidence/.test(content), false,
    filename + ': offline numerical experiments cannot enter the live processor');
}
const offlineNumericalOwners = new Set([
  'src/process/integration/DirectPathCoherentExperiment.h',
  'src/process/integration/CoherentPathEvidence.h',
  'src/process/detection/PostClutterBootstrap.h']);
function guardNativeTree(directory) {
  for (const entry of fs.readdirSync(path.join(root, directory), {withFileTypes: true})) {
    const filename = directory + '/' + entry.name;
    if (entry.isDirectory()) guardNativeTree(filename);
    else if (/\.(?:cpp|h)$/.test(filename) && !offlineNumericalOwners.has(filename)) {
      const source = fs.readFileSync(path.join(root, filename), 'utf8');
      assert.equal(/DirectPathCoherentExperiment|PostClutterBootstrap|CoherentPathEvidence/.test(source), false,
        filename + ': offline numerical owners cannot enter any native runtime include graph');
    }
  }
}
guardNativeTree('src'); guardNativeTree('core');
const offlineBuild = fs.readFileSync(path.join(root, 'cmake/DirectPathCoherentReplay.cmake'), 'utf8');
assert.doesNotMatch(offlineBuild, /install\s*\(/i, 'experimental tools are not installed release targets');
const build = fs.readFileSync(path.join(root, 'CMakeLists.txt'), 'utf8');
assert.match(build, /if\(BUILD_TESTING OR BLAH2_BUILD_OFFLINE_TOOLS\)[\s\S]*?include\(cmake\/DirectPathCoherentReplay.cmake\)/);
const liveTarget = build.split('add_executable(blah2')[1].split('\n)')[0];
assert.doesNotMatch(liveTarget, /Experiment|ReplayReader|PostClutterBootstrap|CoherentPathEvidence/);
const server = fs.readFileSync(path.join(__dirname, 'server.js'), 'utf8');
assert.doesNotMatch(server, /\/api\/(?:tracker\/guided-shadow|bearing\/(?:diagnostic|frequency-resolved-shadow))/);
console.log('Release retirement: canonical 2/4/8, no experiment entrypoints, atomic migration/backup and idempotence passed.');
