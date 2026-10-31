'use strict';
const assert = require('assert');
const {evaluateFrameLiveness} = require('./frame-liveness');

const limits = {staleAfterMs: 1000, startupGraceMs: 2000, cooldownMs: 3000,
  attemptWindowMs: 10000, maxAttempts: 2};
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
