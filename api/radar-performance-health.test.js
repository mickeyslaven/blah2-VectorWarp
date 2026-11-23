'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {RadarPerformanceHealth, trackerCounts, diagnosticCounts} = require('./radar-performance-health');

test('parses compact tracker counters', () => {
  assert.deepEqual(trackerCounts(JSON.stringify({timestamp: 7, n: 35,
    nTentative: 30, nAssociated: 1, nActive: 2, nConfirmed: 1, nCoasting: 1})), {
    timestamp: 7, tentative: 30, associated: 1, active: 2, confirmed: 1,
    coasting: 1, candidates: 31, operational: 3, total: 35
  });
});

test('requires a sustained excessive burden before degrading', () => {
  const health = new RadarPerformanceHealth({window: 6, minimumSamples: 4,
    freshnessMs: Number.MAX_SAFE_INTEGER});
  for (let index = 0; index < 3; index += 1)
    health.accept({timestamp: index + 1, n: 101, nTentative: 100, nAssociated: 0,
      nActive: 1, nConfirmed: 0, nCoasting: 0}, {channel: 'RF20'});
  assert.equal(health.snapshot().status, 'collecting');
  health.accept({timestamp: 4, n: 101, nTentative: 100, nAssociated: 0,
    nActive: 1, nConfirmed: 0, nCoasting: 0}, {channel: 'RF20'});
  assert.equal(health.snapshot().status, 'degraded');
});

test('does not penalize a proportional operational population', () => {
  const health = new RadarPerformanceHealth({window: 6, minimumSamples: 4,
    freshnessMs: Number.MAX_SAFE_INTEGER});
  for (let index = 0; index < 4; index += 1)
    health.accept({timestamp: index + 1, n: 40, nTentative: 32, nAssociated: 0,
      nActive: 8, nConfirmed: 0, nCoasting: 0}, {channel: 'RF20'});
  assert.equal(health.snapshot().status, 'unknown');
});

test('reports independent channel health', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 2,
    freshnessMs: Number.MAX_SAFE_INTEGER});
  for (let index = 0; index < 2; index += 1) {
    health.accept({timestamp: index + 1, n: 3, nTentative: 2, nAssociated: 0,
      nActive: 1, nConfirmed: 0, nCoasting: 0}, {channel: 'RF20'});
    health.accept({timestamp: index + 1, n: 50, nTentative: 50, nAssociated: 0,
      nActive: 0, nConfirmed: 0, nCoasting: 0}, {channel: 'RF21'});
  }
  const result = health.snapshot();
  assert.equal(result.status, 'degraded');
  assert.deepEqual(result.channels.filter(channel => channel.excessive)
    .map(channel => channel.channel), ['RF21']);
});

test('rejects malformed and duplicate frames instead of manufacturing healthy zeros', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 2,
    freshnessMs: Number.MAX_SAFE_INTEGER});
  assert.equal(health.accept({}), false);
  const frame = {timestamp: 1, n: 0, nTentative: 0, nAssociated: 0,
    nActive: 0, nConfirmed: 0, nCoasting: 0};
  assert.equal(health.accept(frame), true);
  assert.equal(health.accept(frame), false);
  assert.equal(health.snapshot().status, 'degraded');
});

test('reports sustained birth rejection as conversion attention, not radar failure', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 2,
    freshnessMs: Number.MAX_SAFE_INTEGER});
  const diagnostics = {detections: 23, birth_candidates: 23,
    birth_snr_rejected: 20, birth_ridge_rejected: 0, initiated: 1,
    associations: 0, promotions: 0, misses: 0, tentative_expired: 0,
    inactivity_deleted: 0};
  for (let index = 0; index < 2; index += 1)
    health.accept({timestamp: index + 1, n: 1, nTentative: 1, nAssociated: 0,
      nActive: 0, nConfirmed: 0, nCoasting: 0}, {channel: 'TX-C', diagnostics});
  const result = health.snapshot();
  assert.equal(result.status, 'attention');
  assert.equal(result.channels[0].conversion.status, 'attention');
  assert.equal(result.channels[0].conversion.rejections.birth_snr_rejected, 40);
  assert.equal(result.channels[0].conversion.rejections.birth_ridge_rejected, 0);
  assert.equal(result.channels[0].conversion.rejections.stationary_birth_rejected, null);
  assert.equal(result.channels[0].conversion.rejectionAccountingCoverage, 0);
  assert.match(result.reasons[0], /SNR\/support 40/);
  assert.match(result.reasons[0], /available for 0% of diagnostic frames/);
  assert.doesNotMatch(result.reasons[0], /calibration/);
});

test('accepts additive canonical diagnostic fields without rewriting them', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 1,
    freshnessMs: Number.MAX_SAFE_INTEGER});
  const diagnostics = {detections: 1, birth_candidates: 1,
    birth_snr_rejected: 0, birth_ridge_rejected: 0, initiated: 1,
    associations: 0, promotions: 0, misses: 0, tentative_expired: 0,
    inactivity_deleted: 0, birth_exclusive_rejected: 1};
  assert.equal(health.accept({timestamp: 1, n: 1, nTentative: 1,
    nAssociated: 0, nActive: 0, nConfirmed: 0, nCoasting: 0}, {diagnostics}), true);
  assert.equal(health.snapshot().channels[0].conversion.diagnosticCoverage, 1);
  assert.equal(health.snapshot().channels[0].conversion.rejections.birth_exclusive_rejected, 1);
});

const completeDiagnostics = overrides => ({detections: 40, birth_candidates: 40,
  birth_snr_rejected: 2, birth_ridge_rejected: 3, birth_doppler_comb_rejected: 4,
  birth_range_comb_rejected: 5, birth_lineage_rejected: 6, birth_capacity_rejected: 7,
  stationary_birth_rejected: 8, stationary_birth_capacity_rejected: 1,
  birth_exclusive_rejected: 2, initiated: 2, associations: 0, promotions: 0,
  misses: 0, tentative_expired: 0, inactivity_deleted: 0, ...overrides});

test('reports every canonical rejection reason and hypothesis initiation semantics', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 1});
  health.accept(nativeFrame(1, 1, 2), {diagnostics: completeDiagnostics()});
  const result = health.snapshot();
  assert.equal(result.schema, 'vectorwarp-radar-performance-health-v3');
  assert.equal(result.channels[0].conversion.rejectionAccountingCoverage, 1);
  assert.deepEqual(result.channels[0].conversion.rejections, {
    birth_snr_rejected: 2, birth_ridge_rejected: 3, birth_doppler_comb_rejected: 4,
    birth_range_comb_rejected: 5, birth_lineage_rejected: 6, birth_capacity_rejected: 7,
    stationary_birth_rejected: 8, stationary_birth_capacity_rejected: 1,
    birth_exclusive_rejected: 2
  });
  assert.equal(result.channels[0].conversion.initiationHypothesesPerCandidate, .05);
  assert.match(result.reasons[0], /2 track hypotheses from 40 unmatched birth candidates/);
  assert.match(result.reasons[0], /stationary 8, stationary capacity 1, exclusive neighborhood 2/);
  assert.doesNotMatch(result.reasons[0], /accounting is available/);
});

test('mixed producer coverage never presents partial rejection sums as complete totals', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 1});
  health.accept(nativeFrame(1, 1, 2), {diagnostics: completeDiagnostics()});
  health.accept(nativeFrame(2, 1, 2), {diagnostics: completeDiagnostics({
    stationary_birth_rejected: undefined})});
  const conversion = health.snapshot().channels[0].conversion;
  assert.equal(conversion.diagnosticCoverage, 1);
  assert.equal(conversion.rejectionAccountingCoverage, .5);
  assert.equal(conversion.rejections.stationary_birth_rejected, null);
  assert.equal(conversion.rejections.birth_range_comb_rejected, 10);
  assert.match(health.snapshot().reasons[0], /available for 50% of diagnostic frames/);
});

test('rejects malformed additive counters without invalidating tracker frame counts', () => {
  for (const value of [-1, .5, NaN, Infinity, null, '1', Number.MAX_SAFE_INTEGER + 1]) {
    const diagnostics = completeDiagnostics({birth_lineage_rejected: value});
    assert.equal(diagnosticCounts(diagnostics), null);
    const health = new RadarPerformanceHealth({minimumSamples: 1});
    assert.equal(health.accept(nativeFrame(1, 1, 2), {diagnostics}), true);
    assert.equal(health.snapshot().channels[0].conversion.diagnosticCoverage, 0);
    assert.equal(health.snapshot().channels[0].conversion.status, 'unknown');
  }
});

test('multiple acceleration hypotheses are not constrained to one initiation per candidate', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 1});
  health.accept(nativeFrame(1, 1, 2), {diagnostics: completeDiagnostics({initiated: 120,
    birth_capacity_rejected: 80})});
  assert.equal(health.snapshot().channels[0].conversion.initiationHypothesesPerCandidate, 3);
  assert.equal(health.snapshot().channels[0].conversion.rejections.birth_capacity_rejected, 80);
});

test('uses local admission time for liveness while preserving producer ordering', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 1, freshnessMs: 1000});
  assert.equal(health.accept({timestamp: 1, n: 0, nTentative: 0,
    nAssociated: 0, nActive: 0, nCoasting: 0}), true);
  const result = health.snapshot();
  assert.notEqual(result.channels[0].input.status, 'stale');
  assert.equal(result.channels[0].latest.timestamp, 1);
});

test('reset establishes a new producer ordering generation', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 1,
    freshnessMs: Number.MAX_SAFE_INTEGER});
  const frame = timestamp => ({timestamp, n: 0, nTentative: 0,
    nAssociated: 0, nActive: 0, nCoasting: 0});
  assert.equal(health.accept(frame(100), {channel: 'TX-C'}), true);
  assert.equal(health.accept(frame(1), {channel: 'TX-C'}), false);
  health.reset();
  assert.equal(health.accept(frame(1), {channel: 'TX-C'}), true);
});

test('an explicit first-frame regression starts a new single-lane generation', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 1,
    freshnessMs: Number.MAX_SAFE_INTEGER});
  const frame = timestamp => ({timestamp, n: 0, nTentative: 0,
    nAssociated: 0, nActive: 0, nCoasting: 0});
  assert.equal(health.accept(frame(100)), true);
  assert.equal(health.accept(frame(1), {resetOnTimestampRegression: true}), true);
  assert.equal(health.snapshot().channels[0].samples, 1);
});

test('a late old-generation replay frame cannot block a lower seek generation', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 1,
    freshnessMs: Number.MAX_SAFE_INTEGER});
  const frame = timestamp => ({timestamp, n: 0, nTentative: 0,
    nAssociated: 0, nActive: 0, nCoasting: 0});
  assert.equal(health.accept(frame(100)), true);
  health.reset();
  assert.equal(health.accept(frame(101)), true); // already-claimed old CPI
  assert.equal(health.accept(frame(5), {resetOnTimestampRegression: true}), true);
  assert.equal(health.snapshot().channels[0].latest.timestamp, 5);
  assert.equal(health.snapshot().channels[0].samples, 1);
});

const nativeFrame = (timestamp, trackerN = 25, candidates = 40, operational = 0) => ({
  timestamp, n: candidates + operational, nTentative: candidates, nAssociated: 0,
  nActive: operational, nConfirmed: 0, nCoasting: 0,
  resolved_product_plan: JSON.stringify({schema: 'vectorwarp-resolved-products-v1',
    trackerM: trackerN < 25 ? 3 : 4, trackerN, integrationCpis: trackerN < 25 ? 1 : 8})
});

test('native confirmation window prevents startup degradation but retains sustained burden', () => {
  const health = new RadarPerformanceHealth();
  for (let index = 1; index < 25; ++index) {
    assert.equal(health.accept(nativeFrame(index * 500)), true);
    assert.equal(health.snapshot().status, 'collecting');
  }
  for (let index = 25; index <= 30; ++index) health.accept(nativeFrame(index * 500));
  const result = health.snapshot();
  assert.equal(result.status, 'degraded');
  assert.equal(result.channels[0].samples, 25);
  assert.deepEqual(result.channels[0].observationWindow,
    {samples: 25, minimumSamples: 25, spanMs: 12000});
  assert.equal(result.limits.window, 25);
  assert.equal(result.limits.minimumSamples, 25);
  assert.equal(result.limits.candidateBurden, 29);
  assert.equal(result.limits.candidateToOperationalRatio, 8);
});

test('a short candidate burst cannot outrun a native confirmation opportunity', () => {
  const health = new RadarPerformanceHealth();
  for (let index = 1; index <= 25; ++index) {
    health.accept(nativeFrame(index * 500, 25, index <= 12 ? 40 : 2, index <= 12 ? 0 : 1));
    assert.notEqual(health.snapshot().status, 'degraded');
  }
  assert.equal(health.snapshot().channels[0].candidateBurden.status, 'healthy');
});

test('native observation windows are per lane and independent of serialization or CPI duration', () => {
  const health = new RadarPerformanceHealth();
  for (let index = 1; index <= 6; ++index) {
    health.accept(nativeFrame(index * 100, 25), {channel: 'deep'});
    health.accept(JSON.stringify(nativeFrame(index * 2000, 6)), {channel: 'short'});
  }
  const channels = health.snapshot().channels;
  assert.equal(channels.find(channel => channel.channel === 'deep').candidateBurden.status, 'collecting');
  const short = channels.find(channel => channel.channel === 'short');
  assert.equal(short.candidateBurden.status, 'degraded');
  assert.deepEqual(short.observationWindow, {samples: 12, minimumSamples: 6, spanMs: 10000});
});

test('invalid or disappearing native plans cannot manufacture legacy health', () => {
  const health = new RadarPerformanceHealth();
  for (const plan of ['{', 'x'.repeat(32769), 'null', JSON.stringify({trackerN: 25}),
    JSON.stringify({schema: 'vectorwarp-resolved-products-v1', trackerN: 256})])
    assert.equal(health.accept({...nativeFrame(1), resolved_product_plan: plan}), false);
  health.reset();
  assert.equal(health.accept(nativeFrame(1)), true);
  const legacy = nativeFrame(2);
  delete legacy.resolved_product_plan;
  assert.equal(health.accept(legacy), false);
  assert.equal(health.snapshot().status, 'degraded');
  assert.equal(health.snapshot().channels[0].samples, 1);
  health.reset();
  assert.equal(health.accept(legacy), true);
});

test('plan changes reset the reporting cohort without accepting older frames or unbounded windows', () => {
  const health = new RadarPerformanceHealth();
  for (let index = 1; index <= 25; ++index) health.accept(nativeFrame(index, 25));
  assert.equal(health.accept(nativeFrame(24, 255)), false);
  assert.equal(health.snapshot().channels[0].samples, 25);
  assert.equal(health.accept(nativeFrame(26, 255)), true);
  assert.equal(health.snapshot().channels[0].samples, 1);
  for (let index = 27; index <= 300; ++index) health.accept(nativeFrame(index, 255));
  assert.equal(health.snapshot().channels[0].samples, 255);
  assert.equal(health.snapshot().status, 'degraded');
});

test('confirmation collection never hides stale or rejected tracker input', () => {
  const originalNow = Date.now;
  let now = 1000;
  Date.now = () => now;
  try {
    const health = new RadarPerformanceHealth({freshnessMs: 100});
    health.accept(nativeFrame(1));
    assert.equal(health.snapshot().status, 'collecting');
    now += 101;
    assert.equal(health.snapshot().status, 'degraded');
    health.accept(nativeFrame(2));
    assert.equal(health.accept(nativeFrame(2)), false);
    assert.equal(health.snapshot().status, 'degraded');
  } finally { Date.now = originalNow; }
});
