'use strict';
const assert = require('assert');
const {createPerformanceHealth} = require('./performance-health');

const frame = (cpi, drops = 0, state = 'ready') => ({cpi,
  acceleration: {state}, captureDroppedSamples: [drops, drops]});

const healthy = createPerformanceHealth(0.5);
for (let i = 0; i < 10; i++) healthy.accept(frame(380, 0));
assert.equal(healthy.status().state, 'healthy');
assert.equal(healthy.status().budgetMs, 500);

const slow = createPerformanceHealth(0.5);
slow.accept(frame(380, 0)); slow.accept(frame(385, 0)); slow.accept(frame(390, 0));
slow.accept(frame(650, 100)); slow.accept(frame(660, 200)); slow.accept(frame(640, 300));
assert.equal(slow.status().state, 'degraded');
assert.equal(slow.status().dropsIncreasing, true);
assert(slow.status().baselineMs < 400);

const coldSlow = createPerformanceHealth(0.5);
for (let i = 0; i < 3; i++) coldSlow.accept(frame(650, i));
assert.equal(coldSlow.status().state, 'degraded', 'severe deadline misses are visible without a fast baseline');

const transient = createPerformanceHealth(0.5);
transient.accept(frame(380)); transient.accept(frame(700)); transient.accept(frame(390));
transient.accept(frame(700)); transient.accept(frame(390));
assert.notEqual(transient.status().state, 'degraded');
transient.accept(frame(1400, 10, 'qualifying'));
assert.equal(transient.status().state, 'warming', 'startup qualification is not a performance regression');
assert.equal(transient.status(false).state, 'unavailable');
transient.reset();
assert.equal(transient.status().latestCpiMs, null);

assert.throws(() => createPerformanceHealth(0), /positive/);
console.log('Hardware-independent performance health: sustained, transient, startup and stale states PASS');
