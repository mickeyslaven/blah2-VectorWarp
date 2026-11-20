'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {compactNodeStatistics, combineNodeStatistics} = require(
  process.env.VECTORWARP_NODE_FEDERATION_MODULE || './node-federation');

function pass(hex, start, channel, detected, tracked) {
  return {hex, started_at: start, finalized_at: start + 10000, channel,
    eligible_cpis: 10, detected_cpis: detected ? 3 : 0,
    tracked_cpis: tracked ? 3 : 0, eligible: true, detected, tracked};
}

function node(id, passes, channelIds) {
  return {id, label: id, available: true, radar: 'receiving', processor_fresh: true,
    config_match: true, tracking_status: 'healthy', passes,
    channels: channelIds.map(channel => ({id: channel, node: id, key: `${id}:${channel}`})),
    timing: {cpi_ms: 430, backlog_ms: 0, drops: [], retired_cpis: 0, retired_samples: 0}};
}

test('explicit association feed replaces the abbreviated scorecard pass list', () => {
  const value = compactNodeStatistics({id: 'beta'},
    {radar: 'receiving', processorFresh: true}, {cpi: 430},
    {current_setup_lifetime: {eligible_passes: 99, detected_passes: 90, tracked_passes: 80},
      recent_scored_passes: [pass('old001', 1000, 'primary', false, false)]},
    {passes: [pass('new001', 2000, 'primary', true, true)]});
  assert.equal(value.passes.length, 1);
  assert.equal(value.passes[0].hex, 'new001');
});

test('headline, nodes, and channels share one common retained finalized window', () => {
  const alpha = node('alpha', [pass('old001', -20000, 'a', true, true),
    pass('same01', 5000, 'a', true, false), pass('same01', 5001, 'b', true, true)], ['a', 'b']);
  const beta = node('beta', [pass('same01', 5002, 'primary', false, false)], ['primary']);
  const result = combineNodeStatistics([alpha, beta]);
  assert.equal(result.statistics_window.started_at, 5002);
  assert.equal(result.system_wide_unique_passes.eligible_adsb_passes, 1);
  assert.equal(result.system_wide_unique_passes.detection.associated_passes, 1);
  assert.equal(result.system_wide_unique_passes.tracking.associated_passes, 1);
  assert.deepEqual(result.channels.map(row => [row.key, row.eligible, row.detected, row.tracked]),
    [['alpha:a', 1, 1, 0], ['alpha:b', 1, 1, 1], ['beta:primary', 1, 0, 0]]);
  assert.deepEqual(result.nodes.map(row => [row.id, row.association]), [
    ['alpha', {eligible: 1, detected: 1, tracked: 1}],
    ['beta', {eligible: 1, detected: 0, tracked: 0}]]);
});
