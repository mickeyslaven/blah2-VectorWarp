'use strict';

const DEFAULTS = Object.freeze({
  staleAfterMs: 30000,
  startupGraceMs: 90000,
  cooldownMs: 120000,
  attemptWindowMs: 900000,
  maxAttempts: 2
});

function finiteTimestamp(value) {
  return Number.isFinite(value) && value >= 0 ? value : null;
}

function evaluateFrameLiveness(input, previous = {}, limits = {}) {
  const policy = {...DEFAULTS, ...limits};
  const now = finiteTimestamp(input.now);
  if (now === null) throw new Error('now must be a non-negative finite timestamp');
  const attempts = Array.isArray(previous.attempts) ? previous.attempts
    .filter(value => finiteTimestamp(value) !== null && now - value < policy.attemptWindowMs) : [];
  const pid = Number.isInteger(input.processorPid) && input.processorPid > 0 ? input.processorPid : 0;
  const pidChanged = pid !== 0 && pid !== previous.processorPid;
  const processorSeenAt = pidChanged ? now : finiteTimestamp(previous.processorSeenAt) ?? now;
  const base = {...previous, processorPid: pid, processorSeenAt, attempts};

  if (!input.processorActive || pid === 0) {
    const unhealthySince = finiteTimestamp(previous.unhealthySince) ?? now;
    const inactive = {...base, status: 'processor-inactive', unhealthySince};
    if (now - unhealthySince < policy.staleAfterMs)
      return {state: inactive, action: null, reason: 'processor-inactive-confirmation'};
    const lastAttemptAt = attempts.length ? attempts[attempts.length - 1] : null;
    if (lastAttemptAt !== null && now - lastAttemptAt < policy.cooldownMs)
      return {state: {...inactive, status: 'cooldown'}, action: null, reason: 'cooldown'};
    if (attempts.length >= policy.maxAttempts)
      return {state: {...inactive, status: 'restart-loop-blocked'}, action: null,
        reason: 'restart-loop-blocked'};
    return {state: {...inactive, status: 'processor-start-requested', attempts: [...attempts, now],
      lastAttemptAt: now}, action: 'start-processor', reason: 'processor-inactive'};
  }
  const lastFrameAt = finiteTimestamp(input.lastFrameAt);
  const advancing = lastFrameAt !== null && lastFrameAt > (finiteTimestamp(previous.lastFrameAt) ?? -1);
  const fresh = lastFrameAt !== null && Math.abs(now - lastFrameAt) <= policy.staleAfterMs;
  if (advancing && fresh)
    return {state: {...base, status: 'healthy', lastFrameAt, unhealthySince: null},
      action: null, reason: 'advancing'};
  if (now - processorSeenAt < policy.startupGraceMs)
    return {state: {...base, status: 'startup-grace', lastFrameAt},
      action: null, reason: 'startup-grace'};

  const unhealthySince = finiteTimestamp(previous.unhealthySince) ?? now;
  const unhealthy = {...base, status: lastFrameAt === null ? 'no-frames' : 'stale-frames',
    lastFrameAt, unhealthySince};
  if (now - unhealthySince < policy.staleAfterMs)
    return {state: unhealthy, action: null, reason: 'confirmation-window'};
  const lastAttemptAt = attempts.length ? attempts[attempts.length - 1] : null;
  if (lastAttemptAt !== null && now - lastAttemptAt < policy.cooldownMs)
    return {state: {...unhealthy, status: 'cooldown'}, action: null, reason: 'cooldown'};
  if (attempts.length >= policy.maxAttempts)
    return {state: {...unhealthy, status: 'restart-loop-blocked'}, action: null,
      reason: 'restart-loop-blocked'};
  return {state: {...unhealthy, status: 'recovery-requested', attempts: [...attempts, now],
    lastAttemptAt: now}, action: 'recover', reason: unhealthy.status};
}

module.exports = {DEFAULTS, evaluateFrameLiveness};
