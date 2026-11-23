'use strict';

const DEFAULTS = Object.freeze({
  staleAfterMs: 30000,
  startupGraceMs: 90000,
  maxStartupGraceMs: 600000,
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
  const processorProgress = finiteTimestamp(input.processorProgress);
  const previousProgress = finiteTimestamp(previous.processorProgress);
  const progressAdvanced = !pidChanged && processorProgress !== null && previousProgress !== null &&
    processorProgress > previousProgress;
  const progressSeenAt = progressAdvanced ? now : pidChanged ? null :
    finiteTimestamp(previous.progressSeenAt);
  const lastFrameAt = finiteTimestamp(input.lastFrameAt);
  // An old API timestamp is not a published frame from a new processor.  Once
  // this processor publishes, losing API telemetry must not reopen startup.
  const previousFrameAt = finiteTimestamp(previous.lastFrameAt);
  const processorHasFrame = (!pidChanged && (previous.processorHasFrame === true ||
    (previousFrameAt !== null && previousFrameAt >= processorSeenAt))) ||
    (lastFrameAt !== null && lastFrameAt >= processorSeenAt);
  const base = {...previous, processorPid: pid, processorSeenAt, processorProgress,
    progressSeenAt, processorHasFrame, attempts,
    unhealthySince: pidChanged ? null : previous.unhealthySince};

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
  const advancing = lastFrameAt !== null && lastFrameAt > (previousFrameAt ?? -1);
  const fresh = lastFrameAt !== null && Math.abs(now - lastFrameAt) <= policy.staleAfterMs;
  if (advancing && fresh && processorHasFrame)
    return {state: {...base, status: 'healthy', lastFrameAt, unhealthySince: null},
      action: null, reason: 'advancing'};
  if (!processorHasFrame && now - processorSeenAt < policy.startupGraceMs)
    return {state: {...base, status: 'startup-grace', lastFrameAt, unhealthySince: null},
      action: null, reason: 'startup-grace'};
  // Private GPU qualification can legitimately take longer than the fixed
  // grace on slower supported hardware.  Keep waiting only while this exact
  // processor consumes CPU and only within a hard upper bound; a stalled or
  // runaway process still reaches the normal bounded recovery path.
  if (!processorHasFrame && now - processorSeenAt < policy.maxStartupGraceMs &&
      processorProgress !== null && progressSeenAt !== null &&
      now - progressSeenAt <= policy.staleAfterMs)
    return {state: {...base, status: 'startup-progress', lastFrameAt, unhealthySince: null},
      action: null, reason: 'startup-progress'};

  const unhealthySince = finiteTimestamp(base.unhealthySince) ?? now;
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
