'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {RadarPerformanceHealth, trackerCounts} = require('./radar-performance-health');

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
      nActive: 0, nConfirmed: 0, nCoasting: 0}, {channel: 'WPXI', diagnostics});
  const result = health.snapshot();
  assert.equal(result.status, 'attention');
  assert.equal(result.channels[0].conversion.status, 'attention');
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
  assert.equal(health.accept(frame(100), {channel: 'WPXI'}), true);
  assert.equal(health.accept(frame(1), {channel: 'WPXI'}), false);
  health.reset();
  assert.equal(health.accept(frame(1), {channel: 'WPXI'}), true);
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
