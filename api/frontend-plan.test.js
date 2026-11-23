'use strict';
// API/native parity and extraction negative controls; native executable is
// supplied by CTest, never synthesized from the API's expected identity.
const assert = require('node:assert/strict');
const {test} = require('node:test');
const {spawnSync} = require('node:child_process');
const {configurationForReceiver} = require('./config-store');
const {effectiveRadarPlan, radarPlanErrors} = require('./config-manager');
const clone = value => JSON.parse(JSON.stringify(value));
function configured(channelized) {
  const config = configurationForReceiver('Usrp');
  config.capture.fs = channelized ? 24000000 : 2000000;
  config.capture.fc = channelized ? 650000000 : 100000000;
  config.process.multi_illuminator = {enabled: channelized, illuminators: channelized ? [{
    enabled: true, id: 'tv', standard: 'DVB-T2', frequency_hz: 652000000,
    occupied_bandwidth_hz: 7000000, output_sample_rate_hz: 8000000, filter_taps_per_phase: 64,
    atsc_reference_mode: 'raw'}] : []};
  return config;
}
function pin(report) {
  return {schema: 'vectorwarp-radar-plan-requirements-v1', semantic_sha256: report.semantic_sha256,
    frontend_sha256: report.frontend_sha256, required_features: {detection: true}};
}
test('frontend independently binds extraction while downstream semantic identity stays stable', () => {
  const config = configured(true), original = effectiveRadarPlan(config);
  for (const change of [c => { c.capture.fs *= 2; }, c => { c.capture.fc += 1000000; },
    c => { c.process.multi_illuminator.illuminators[0].occupied_bandwidth_hz = 1000000; },
    c => { c.process.multi_illuminator.illuminators[0].filter_taps_per_phase = 4; },
    c => { c.process.multi_illuminator.illuminators[0].standard = 'DVB-T'; }]) {
    const candidate = clone(config); change(candidate);
    const report = effectiveRadarPlan(candidate);
    assert.equal(report.semantic_sha256, original.semantic_sha256);
    assert.notEqual(report.frontend_sha256, original.frontend_sha256);
    assert.ok(radarPlanErrors(report, pin(original)).some(message => /frontend/.test(message)));
  }
  const oldPin = pin(original); delete oldPin.frontend_sha256;
  assert.ok(radarPlanErrors(original, oldPin).some(message => /review/.test(message)));
  const direct = configured(false), channelized = clone(direct);
  channelized.process.multi_illuminator = {enabled: true, illuminators: [{enabled: true,
    id: 'primary', standard: 'FM', frequency_hz: direct.capture.fc,
    occupied_bandwidth_hz: 200000, output_sample_rate_hz: direct.capture.fs}]};
  assert.equal(effectiveRadarPlan(direct).semantic_sha256, effectiveRadarPlan(channelized).semantic_sha256);
  assert.notEqual(effectiveRadarPlan(direct).frontend_sha256, effectiveRadarPlan(channelized).frontend_sha256);
});
test('live/replay and backend select one frontend contract; recording metadata remains bound', () => {
  for (const channelized of [false, true]) {
    const live = configured(channelized), expected = effectiveRadarPlan(live);
    for (const backend of ['cpu', 'gpu', 'gpu-resident', 'auto']) {
      const replay = clone(live);
      Object.assign(replay.capture.replay, {state: true, sample_rate_hz: live.capture.fs,
        frequency_hz: live.capture.fc, channel_count: 2, reference_channel: 0,
        reference_mode: 'dedicated', reference_channels: [0], surveillance_channels: [1]});
      replay.process.performance.acceleration = backend;
      assert.equal(effectiveRadarPlan(replay).frontend_sha256, expected.frontend_sha256);
      replay.capture.replay.frequency_hz += 1000000;
      assert.notEqual(effectiveRadarPlan(replay).frontend_sha256, expected.frontend_sha256);
      assert.ok(radarPlanErrors(effectiveRadarPlan(replay), pin(expected)).length);
    }
    const remapped = clone(live);
    Object.assign(remapped.capture.replay, {state: true, channel_count: 2,
      reference_channel: 1, reference_channels: [1], reference_mode: 'dedicated', surveillance_channels: [0]});
    assert.notEqual(effectiveRadarPlan(remapped).frontend_sha256, expected.frontend_sha256);
  }
});
if (process.env.VECTORWARP_FRONTEND_PLAN_TEST) test('native serialization and publication match API plans byte for byte', () => {
  const native = spawnSync(process.env.VECTORWARP_FRONTEND_PLAN_TEST, [], {encoding: 'utf8', timeout: 5000});
  assert.equal(native.status, 0, native.stderr);
  const frames = native.stdout.trim().split('\n').map(line => JSON.parse(line));
  assert.equal(frames.length, 3);
  const configs = [configured(false), configured(true), configured(true)];
  configs[2].process.multi_illuminator.illuminators[0].filter_taps_per_phase = 5;
  frames.forEach((frame, index) => {
    assert.equal(frame.resolved_frontend_plan, JSON.stringify(effectiveRadarPlan(configs[index]).lanes[0].frontend));
    assert.equal(frame.input_lineage.firstInputSampleExact, '9007199254740993');
  });
});
