'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const {compactNodeStatistics, combineNodeStatistics, probeJson} = require(
  process.env.VECTORWARP_NODE_FEDERATION_MODULE || './node-federation');
const {federationAssociationPassTransport} = require('./evidence-statistics');
const coverage = require('../html/js/measured_track_coverage');

test('large association reads require an explicit bounded allowance', async t => {
  const body = JSON.stringify({passes: ['x'.repeat(2 * 1024 * 1024)]});
  const server = http.createServer((_request, response) => {
    response.writeHead(200, {'Content-Type': 'application/json'});
    response.end(body);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  const origin = `http://127.0.0.1:${server.address().port}`;
  await assert.rejects(probeJson(origin, '/association', 2500),
    /response is too large/);
  await assert.rejects(probeJson(origin, '/association', 2500, 1024 * 1024), /response is too large/);
  const accepted = await probeJson(origin, '/association', 2500, 8 * 1024 * 1024);
  assert.equal(accepted.value.passes[0].length, 2 * 1024 * 1024);
  await assert.rejects(probeJson(origin, '/association', 2500, 17 * 1024 * 1024),
    /byte limit is invalid/);
});

test('network summary uses the bounded association basis contract', () => {
  const source = require('node:fs').readFileSync(
    require.resolve(process.env.VECTORWARP_NODE_FEDERATION_MODULE || './node-federation'),
    'utf8');
  assert.match(source,
    /association-basis\?timing=provisional&compact=true'[\s\S]*?timeoutMs, 1024 \* 1024/,
    'network polling must not transfer the full measured-track evidence ledger');
  assert.match(source,
    /association\/passes\?timing=provisional&transport=federation-v1'[\s\S]*?timeoutMs,\s*1024 \* 1024/,
    'network polling must request the bounded columnar recent-pass transport');
});

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
    {radar: 'receiving', processorFresh: true, processor: {state: 'live'},
      configRevision: 'a', loadedRevision: 'a',
      featureDiagnostics: {trackingPerformance: {status: 'healthy'}, evidenceStatistics: {
        status: 'degraded', channels: [{id: 'primary', errors: ['ledger unwritable']}]}}},
    {cpi: 430, capture_backlog_ms: 500, captureDroppedSamples: [2, 2]},
    {current_setup_lifetime: {eligible_passes: 10, detected_passes: 4, tracked_passes: 3},
      recent_scored_passes: []});
  assert.deepEqual(value.association, {eligible: 10, detected: 4, tracked: 3});
  assert.equal(value.channels[0].key, 'beta:primary');
  assert.equal(value.timing.backlog_ms, 500);
  assert.equal(value.timing.raw_capture_backlog_ms, 500);
  assert.equal(value.evidence_status, 'degraded');
  assert.deepEqual(value.evidence_errors, ['ledger unwritable']);
  assert.equal(combineNodeStatistics([{...value, passes: []}]).health.status, 'degraded');
});

test('reports queued work rather than the receiver in-flight CPI', () => {
  const status = {radar: 'receiving', processorFresh: true, processor: {state: 'live'},
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

test('display-only coordinators are not counted as radar nodes', () => {
  const value = compactNodeStatistics({id: 'display', label: 'Display', local: true, role: 'coordinator'},
    {radar: 'no-data', processorFresh: false, processor: null}, {},
    {current_setup_lifetime: {}, recent_scored_passes: []});
  assert.equal(value.available, false);
  const result = combineNodeStatistics([value, node('radar', 1, 1, 1,
    {hex: 'abc123', started_at: 1, finalized_at: 2, eligible_cpis: 3})]);
  assert.equal(result.health.status, 'healthy');
  assert.equal(result.health.nodes_total, 1);
  assert.deepEqual(result.health.coordinator_nodes, ['display']);
  const failedRadar = compactNodeStatistics({id: 'failed', local: true},
    {radar: 'no-data', processorFresh: false, processor: null}, {}, {});
  assert.deepEqual(combineNodeStatistics([failedRadar]).health.unhealthy_nodes, ['failed']);
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

test('does not mix retained history with a restarted node awaiting its first pass', () => {
  const retained = {hex: 'abc123', started_at: 100000, finalized_at: 120000,
    outcome: 'tracked', eligible_cpis: 20, detected_cpis: 10, tracked_cpis: 5};
  const restarted = node('beta', 0, 0, 0, retained);
  restarted.passes = [];
  const result = combineNodeStatistics([node('alpha', 100, 80, 70, retained), restarted]);
  assert.equal(result.statistics_window.alignment_pending, true);
  assert.equal(result.statistics_window.started_at, null);
  assert.deepEqual(result.channel_opportunities, {eligible: 0, detected: 0, tracked: 0,
    detection_yield: null, tracking_yield: null, tracked_of_eligible_yield: null});
  assert.equal(result.system_wide_unique_passes.eligible_adsb_passes, 0);
  assert.equal(result.system_wide_unique_passes.alignment_pending, true);
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

test('cached statistics remain visible without certifying stale node health', () => {
  const cached = {...node('alpha', 1, 1, 1, {hex: 'a00001', started_at: 1,
    finalized_at: 2, eligible_cpis: 3, outcome: 'tracked'}),
    stale: true, stale_reason: 'request timed out'};
  const result = combineNodeStatistics([cached]);
  assert.equal(result.system_wide_unique_passes.eligible_adsb_passes, 1);
  assert.equal(result.nodes[0].stale, true);
  assert.equal(result.health.status, 'degraded');
  assert.deepEqual(result.health.unhealthy_nodes, ['alpha']);
});

test('compact and deduplicated network statistics preserve scoring and restart provenance', () => {
  const pass = {pass_id: 'pass-1', hex: 'abc123', started_at: 1000, finalized_at: 2000,
    channel: 'primary', eligible_cpis: 6, detected_cpis: 3, detected: true,
    site_epoch: 'site', evaluation_contract_hash: 'contract', evaluation_policy_hash: 'a'.repeat(64),
    timing_policy: 'provisional', decision_grade: false,
    sensitivity_epoch_id: null, sensitivity_epoch_mixed: true};
  const compact = compactNodeStatistics({id: 'alpha'},
    {radar: 'receiving', processorFresh: true, processor: {state: 'live'}}, {},
    {timing_policy: 'provisional', channels: [{selected_channel: 'primary', association_statistics: {
      evaluation_contract_hash: 'contract', evaluation_policy_hash: 'a'.repeat(64),
      timing_policy: 'provisional', decision_grade: false, sensitivity_epoch: {id: 'new-run'}}}]},
    {timing_policy: 'provisional', statistics_window: {complete: false}, passes: [pass]});
  const combined = combineNodeStatistics([compact]);
  assert.equal(combined.nodes[0].pass_count, 1);
  assert.equal(combined.channels[0].evaluation_policy_hash, 'a'.repeat(64));
  const provenance = combined.unique_recent_passes.passes[0].provenance[0];
  assert.equal(provenance.node, 'alpha');
  assert.equal(provenance.sensitivity_epoch_mixed, true);
  assert.equal(provenance.decision_grade, false);
  assert.equal(combined.evaluation.mixed_epoch_pass_rows, 1);
  assert.equal(combined.evaluation.decision_grade, false);
  assert.equal(combined.evaluation.complete_24h_window, false);
  assert.equal(combined.evaluation.policy_compatible, true);
  const other = structuredClone(compact);
  other.id = 'beta'; other.channels[0].evaluation_policy_hash = 'different-policy';
  assert.equal(combineNodeStatistics([compact, other]).evaluation.policy_compatible, false);
});

test('columnar node transport preserves network outcomes without returning node ledgers', () => {
  const passes = [{hex: 'abc123', started_at: 1000, finalized_at: 2000,
    outcome: 'tracked', channel: 'primary', eligible_cpis: 6, detected_cpis: 4,
    tracked_cpis: 3, eligible: true, detected: true, tracked: true},
  {hex: 'def456', started_at: 3000, finalized_at: 4000,
    outcome: 'no_radar_return', channel: 'primary', eligible_cpis: 6,
    eligible: true, detected: false, tracked: false}];
  const compact = compactNodeStatistics({id: 'alpha'},
    {radar: 'receiving', processorFresh: true, processor: {state: 'live'}}, {},
    {timing_policy: 'provisional', channels: [{selected_channel: 'primary',
      association_statistics: {evaluation_policy_hash: 'p'}}]},
    {timing_policy: 'provisional', statistics_window: {complete: true},
      transport: federationAssociationPassTransport(passes)});
  const combined = combineNodeStatistics([compact]);
  assert.equal(combined.system_wide_unique_passes.eligible_adsb_passes, 2);
  assert.equal(combined.system_wide_unique_passes.detection.associated_passes, 1);
  assert.equal(combined.system_wide_unique_passes.tracking.associated_passes, 1);
  assert.equal(combined.nodes[0].pass_count, 2);
  assert.equal(combined.nodes[0].passes, undefined);
});

test('compact coverage transport yields exact cross-node union and fails closed without its basis', () => {
  const nodes = [[[0, 10000], [30000, 40000]], [[5000, 20000], [35000, 50000]]].map((measured, index) => {
    const basis = {version: 1, complete: true, aircraft: [{hex: index ? 'ABC123' : 'abc123',
      eligible: [[0, 100000]], scored: [[0, 100000]], measured}]};
    return compactNodeStatistics({id: `radar-${index}`},
      {radar: 'receiving', processorFresh: true, processor: {state: 'live'}}, {},
      {channels: [{selected_channel: 'primary', association_statistics: {evaluation_policy_hash: 'a'.repeat(64)},
        measured_track_basis: coverage.compactBasis(basis), measured_track_coverage: coverage.aggregate([basis]),
        cpi_exposure: {eligible_cpis: 200, operationally_tracked_cpis: 80, eligible_seconds: 100}}]},
      {passes: [{hex: 'abc123', started_at: 1, finalized_at: 100000, eligible_cpis: 200}]});
  });
  const result = combineNodeStatistics([...nodes, {id: 'display', role: 'coordinator', available: false}]);
  assert.equal(result.measured_track_coverage.available, true);
  assert.equal(result.measured_track_coverage.fraction, .4, 'Overlapping support is unioned, not added');
  assert.equal(result.measured_track_coverage.eligible_seconds, 100);
  assert.equal(result.measured_track_coverage.measured_seconds, 40);
  assert.equal(result.measured_track_coverage.gap_count, 1);
  assert.equal(result.measured_track_coverage.gap_max_seconds, 10);
  assert.equal(result.channels[0].measured_track_coverage.measured_seconds, 20);
  assert.equal(result.channels[0].cpi_exposure.operationally_tracked_cpis, 80);
  assert.equal(result.channels[0].cpi_exposure.eligible_seconds, 100);
  assert.equal(JSON.stringify(result).includes('measured_track_basis'), false);
  const partial = structuredClone(nodes); partial[0].channels[0].measured_track_basis.complete = false;
  assert.equal(combineNodeStatistics(partial).measured_track_coverage.scored_fraction, .4);
  for (const mutate of [rows => { delete rows[1].channels[0].measured_track_basis; },
    rows => { rows[1].channels[0].measured_track_basis.aircraft[0].measured = [0, -1]; },
    rows => { rows[1].available = false; }, rows => { rows[1].stale = true; },
    rows => { rows[1].channels[0].evaluation_policy_hash = 'b'.repeat(64); },
    rows => { rows.forEach(node => { node.channels[0].evaluation_policy_hash = 'unidentified'; }); }]) {
    const changed = structuredClone(nodes); mutate(changed);
    const unavailable = combineNodeStatistics(changed).measured_track_coverage;
    assert.equal(unavailable.available, false);
    for (const field of ['fraction', 'scored_fraction', 'measured_seconds', 'scored_seconds', 'gap_count', 'observed_internal_gap_count'])
      assert.equal(unavailable[field], null, `${field} must not fabricate zero or partial coverage`);
  }
});
