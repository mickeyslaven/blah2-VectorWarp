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
  const health = new RadarPerformanceHealth({window: 6, minimumSamples: 4});
  for (let index = 0; index < 3; index += 1)
    health.accept({nTentative: 100, nActive: 1}, {channel: 'RF20'});
  assert.equal(health.snapshot().status, 'collecting');
  health.accept({nTentative: 100, nActive: 1}, {channel: 'RF20'});
  assert.equal(health.snapshot().status, 'degraded');
});

test('does not penalize a proportional operational population', () => {
  const health = new RadarPerformanceHealth({window: 6, minimumSamples: 4});
  for (let index = 0; index < 4; index += 1)
    health.accept({nTentative: 32, nActive: 8}, {channel: 'RF20'});
  assert.equal(health.snapshot().status, 'healthy');
});

test('reports independent channel health', () => {
  const health = new RadarPerformanceHealth({minimumSamples: 2});
  for (let index = 0; index < 2; index += 1) {
    health.accept({nTentative: 2, nActive: 1}, {channel: 'RF20'});
    health.accept({nTentative: 50, nActive: 0}, {channel: 'RF21'});
  }
  const result = health.snapshot();
  assert.equal(result.status, 'degraded');
  assert.deepEqual(result.channels.filter(channel => channel.excessive)
    .map(channel => channel.channel), ['RF21']);
});
