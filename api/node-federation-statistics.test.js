'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {compactNodeStatistics, combineNodeStatistics} = require(
  process.env.VECTORWARP_NODE_FEDERATION_MODULE || './node-federation');

function node(id, eligible, detected, tracked, pass) {
  return {id, label: id.toUpperCase(), local: id === 'alpha', available: true,
    radar: 'receiving', processor_fresh: true, config_match: true,
    tracking_status: 'healthy', channels: [{id: 'primary', node: id, key: `${id}:primary`,
      eligible, detected, tracked}], association: {eligible, detected, tracked},
    passes: [pass], timing: {cpi_ms: id === 'alpha' ? 430 : 440,
      backlog_ms: id === 'alpha' ? 8 : 500, drops: [0], retired_cpis: 0, retired_samples: 0}};
}

test('compacts single-node scorecard with provenance', () => {
  const value = compactNodeStatistics({id: 'beta', label: 'Remote Node', local: false},
    {radar: 'receiving', processorFresh: true, configRevision: 'a', loadedRevision: 'a',
      featureDiagnostics: {trackingPerformance: {status: 'healthy'}}},
    {cpi: 430, capture_backlog_ms: 500, captureDroppedSamples: [2, 2]},
    {current_setup_lifetime: {eligible_passes: 10, detected_passes: 4, tracked_passes: 3},
      recent_scored_passes: []});
  assert.deepEqual(value.association, {eligible: 10, detected: 4, tracked: 3});
  assert.equal(value.channels[0].key, 'beta:primary');
  assert.equal(value.timing.backlog_ms, 500);
  assert.equal(value.timing.raw_capture_backlog_ms, 500);
});

test('reports queued work rather than the receiver in-flight CPI', () => {
  const status = {radar: 'receiving', processorFresh: true,
    featureDiagnostics: {processing: {configuredCpiMs: 500}}};
  const scorecard = {current_setup_lifetime: {}, recent_scored_passes: []};
  const staged = compactNodeStatistics({id: 'beta'}, status,
    {cpi: 430, capture_backlog_ms: 504.3}, scorecard);
  assert.ok(Math.abs(staged.timing.backlog_ms - 4.3) < 1e-9);
  assert.equal(staged.timing.raw_capture_backlog_ms, 504.3);
  const explicit = compactNodeStatistics({id: 'alpha'}, status,
    {cpi: 430, capture_backlog_ms: 910, capture_queue_ready_cpis: 1}, scorecard);
  assert.equal(explicit.timing.backlog_ms, 500);
});

test('combines aligned node opportunities and deduplicates aircraft passes', () => {
  const passA = {hex: 'abc123', started_at: 100000, finalized_at: 120000,
    outcome: 'detected_not_tracked', eligible_cpis: 10, detected_cpis: 2, tracked_cpis: 0};
  const passB = {...passA, started_at: 105000, outcome: 'tracked', detected_cpis: 0, tracked_cpis: 3};
  const result = combineNodeStatistics([node('alpha', 20, 12, 8, passA),
    node('beta', 10, 3, 2, passB)], {available: true,
      tracks: [{state: 'COOPERATIVE_ACTIVE'}, {state: 'COOPERATIVE_CANDIDATE'}]});
  assert.deepEqual(result.channel_opportunities, {eligible: 2, detected: 2, tracked: 1,
    detection_yield: 1, tracking_yield: .5, tracked_of_eligible_yield: .5});
  assert.equal(result.unique_recent_passes.eligible, 1);
  assert.equal(result.unique_recent_passes.detected, 1);
  assert.equal(result.unique_recent_passes.tracked, 1);
  assert.deepEqual(result.unique_recent_passes.passes[0].nodes, ['alpha', 'beta']);
  assert.equal(result.system_wide_unique_passes.detection.yield, 1);
  assert.equal(result.system_wide_unique_passes.tracking.yield, 1);
  assert.equal(result.cooperative_tracks.active, 1);
  assert.equal(result.health.status, 'healthy');
});

test('unique pass yield uses finalized qualification rather than any positive CPI', () => {
  const weak = {hex: 'abc123', started_at: 100000, finalized_at: 120000,
    outcome: 'no_radar_return', eligible_cpis: 10, detected_cpis: 2, tracked_cpis: 1};
  const result = combineNodeStatistics([node('alpha', 1, 0, 0, weak)]);
  assert.equal(result.unique_recent_passes.eligible, 1);
  assert.equal(result.unique_recent_passes.detected, 0);
  assert.equal(result.unique_recent_passes.tracked, 0);
});

test('network health degrades when any registered node is unavailable', () => {
  const good = node('alpha', 1, 1, 1, {hex: 'a00001', started_at: 1, eligible_cpis: 3});
  const result = combineNodeStatistics([good, {id: 'beta', available: false}]);
  assert.equal(result.health.status, 'degraded');
  assert.deepEqual(result.health.unhealthy_nodes, ['beta']);
});
