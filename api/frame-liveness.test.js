'use strict';
const assert = require('assert');
const {evaluateFrameLiveness} = require('./frame-liveness');

const limits = {staleAfterMs: 1000, startupGraceMs: 2000, cooldownMs: 3000,
  maxStartupGraceMs: 6000, attemptWindowMs: 10000, maxAttempts: 2};
let state = {};
let result = evaluateFrameLiveness({now: 0, processorActive: true, processorPid: 10,
  lastFrameAt: null}, state, limits);
assert.equal(result.reason, 'startup-grace');
state = result.state;
result = evaluateFrameLiveness({now: 2100, processorActive: true, processorPid: 10,
  lastFrameAt: null}, state, limits);
assert.equal(result.reason, 'confirmation-window');
state = result.state;
result = evaluateFrameLiveness({now: 3200, processorActive: true, processorPid: 10,
  lastFrameAt: null}, state, limits);
assert.equal(result.action, 'recover', 'zero-frame active processor must recover');
state = result.state;
result = evaluateFrameLiveness({now: 3500, processorActive: true, processorPid: 10,
  lastFrameAt: null}, state, limits);
assert.equal(result.reason, 'cooldown');

result = evaluateFrameLiveness({now: 6300, processorActive: true, processorPid: 10,
  lastFrameAt: null}, result.state, limits);
assert.equal(result.action, 'recover');
result = evaluateFrameLiveness({now: 9400, processorActive: true, processorPid: 10,
  lastFrameAt: null}, result.state, limits);
assert.equal(result.reason, 'restart-loop-blocked', 'bounded recovery must not loop');

state = {};
result = evaluateFrameLiveness({now: 0, processorActive: true, processorPid: 20,
  processorProgress: 10, lastFrameAt: null}, state, limits);
state = result.state;
result = evaluateFrameLiveness({now: 2100, processorActive: true, processorPid: 20,
  processorProgress: 20, lastFrameAt: null}, state, limits);
assert.equal(result.reason, 'startup-progress', 'advancing private qualification must not be killed');
state = result.state;
result = evaluateFrameLiveness({now: 3200, processorActive: true, processorPid: 20,
  processorProgress: 20, lastFrameAt: null}, state, limits);
assert.equal(result.reason, 'confirmation-window', 'stalled private qualification must become unhealthy');
state = result.state;
result = evaluateFrameLiveness({now: 4301, processorActive: true, processorPid: 20,
  processorProgress: 20, lastFrameAt: null}, state, limits);
assert.equal(result.action, 'recover', 'stalled private qualification must recover');

// CPU activity may extend startup, but never beyond the absolute startup cap.
state = evaluateFrameLiveness({now: 0, processorActive: true, processorPid: 21,
  processorProgress: 10, lastFrameAt: null}, {}, limits).state;
for (const now of [2100, 3200, 4300, 5900]) {
  result = evaluateFrameLiveness({now, processorActive: true, processorPid: 21,
    processorProgress: now, lastFrameAt: null}, state, limits);
  assert.equal(result.reason, 'startup-progress');
  state = result.state;
}
result = evaluateFrameLiveness({now: 6000, processorActive: true, processorPid: 21,
  processorProgress: 6000, lastFrameAt: null}, state, limits);
assert.equal(result.reason, 'confirmation-window', 'CPU spin cannot extend the hard startup cap');
result = evaluateFrameLiveness({now: 7101, processorActive: true, processorPid: 21,
  processorProgress: 7101, lastFrameAt: null}, result.state, limits);
assert.equal(result.action, 'recover', 'runaway startup must recover despite continuing CPU activity');

// A new process must not inherit an expired unhealthy interval or an old frame.
state = {processorPid: 22, processorSeenAt: 0, lastFrameAt: 1000,
  processorHasFrame: true, unhealthySince: 1000, processorProgress: 100};
result = evaluateFrameLiveness({now: 5000, processorActive: true, processorPid: 23,
  processorProgress: 1, lastFrameAt: 1000}, state, limits);
assert.equal(result.reason, 'startup-grace');
assert.equal(result.state.unhealthySince, null);
result = evaluateFrameLiveness({now: 7100, processorActive: true, processorPid: 23,
  processorProgress: 2, lastFrameAt: 1000}, result.state, limits);
assert.equal(result.reason, 'startup-progress', 'a retained old-PID frame cannot cancel startup');
result = evaluateFrameLiveness({now: 8201, processorActive: true, processorPid: 23,
  processorProgress: 2, lastFrameAt: null}, result.state, limits);
assert.equal(result.reason, 'confirmation-window');
result = evaluateFrameLiveness({now: 8300, processorActive: true, processorPid: 23,
  processorProgress: 3, lastFrameAt: null}, result.state, limits);
assert.equal(result.reason, 'startup-progress');
assert.equal(result.state.unhealthySince, null, 'resumed startup clears the previous stall interval');
result = evaluateFrameLiveness({now: 9401, processorActive: true, processorPid: 23,
  processorProgress: 3, lastFrameAt: null}, result.state, limits);
assert.equal(result.reason, 'confirmation-window', 'a new stall gets a full confirmation window');

// Missing/invalid process counters cannot invent startup progress on upgrade.
for (const processorProgress of [null, undefined, NaN, -1]) {
  result = evaluateFrameLiveness({now: 5000, processorActive: true, processorPid: 24,
    processorProgress, lastFrameAt: null}, {processorPid: 24, processorSeenAt: 0}, limits);
  assert.equal(result.reason, 'confirmation-window');
}

// Publishing is irreversible for one PID, even if the API then loses its socket.
state = evaluateFrameLiveness({now: 0, processorActive: true, processorPid: 25,
  processorProgress: 1, lastFrameAt: null}, {}, limits).state;
state = evaluateFrameLiveness({now: 100, processorActive: true, processorPid: 25,
  processorProgress: 2, lastFrameAt: 100}, state, limits).state;
result = evaluateFrameLiveness({now: 500, processorActive: true, processorPid: 25,
  processorProgress: 3, lastFrameAt: null}, state, limits);
assert.equal(result.reason, 'confirmation-window', 'published PID cannot regain fixed startup grace');
result = evaluateFrameLiveness({now: 2500, processorActive: true, processorPid: 25,
  processorProgress: 4, lastFrameAt: null}, result.state, limits);
assert.equal(result.action, 'recover', 'published PID cannot regain progress-based startup grace');
result = evaluateFrameLiveness({now: 3000, processorActive: true, processorPid: 25,
  processorProgress: 10, lastFrameAt: null}, {processorPid: 25, processorSeenAt: 0,
  lastFrameAt: 2000, processorProgress: 9}, limits);
assert.equal(result.reason, 'confirmation-window', 'old state schemas retain known publication');

state = {processorPid: 11, processorSeenAt: 0, lastFrameAt: 1000};
result = evaluateFrameLiveness({now: 5000, processorActive: true, processorPid: 11,
  lastFrameAt: 1000}, state, limits);
assert.equal(result.reason, 'confirmation-window', 'stale frame needs confirmation');
result = evaluateFrameLiveness({now: 6100, processorActive: true, processorPid: 11,
  lastFrameAt: 1000}, result.state, limits);
assert.equal(result.action, 'recover', 'simulated receiver reset must recover');
result = evaluateFrameLiveness({now: 6200, processorActive: true, processorPid: 12,
  lastFrameAt: 6200}, result.state, limits);
assert.equal(result.reason, 'advancing');
assert.equal(result.state.unhealthySince, null);

state = {processorPid: 12, processorSeenAt: 6200, lastFrameAt: 6200, attempts: []};
result = evaluateFrameLiveness({now: 7000, processorActive: false, processorPid: 0,
  lastFrameAt: null}, state, limits);
assert.equal(result.reason, 'processor-inactive-confirmation', 'brief service transitions need confirmation');
result = evaluateFrameLiveness({now: 8101, processorActive: false, processorPid: 0,
  lastFrameAt: null}, result.state, limits);
assert.equal(result.action, 'start-processor', 'an inactive live processor must recover after calibration');
result = evaluateFrameLiveness({now: 8200, processorActive: false, processorPid: 0,
  lastFrameAt: null}, result.state, limits);
assert.equal(result.reason, 'cooldown', 'an inactive processor cannot be restarted in a tight loop');
console.log('PASS advancing-frame liveness, receiver reset recovery and loop protection');
