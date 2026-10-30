'use strict';

const path = require('path');

function installedGainRestartSupported(command, {platform = process.platform} = {}) {
  return platform === 'linux' && Array.isArray(command) && command.length === 2 &&
    typeof command[0] === 'string' && path.isAbsolute(command[0]) &&
    path.basename(command[0]) === 'vectorwarp-receiver-helper' && command[1] === 'request-restart';
}

// The restart receipt passed here must come from readSdrplayStartup, which
// checks the installed Linux helper's root-owned file. Processor telemetry by
// itself cannot prove that tracking/filter state was discarded by a restart.
function pending(state) {
  const gain = state?.receipt?.automaticGain;
  return !!gain && Object.prototype.hasOwnProperty.call(gain, 'restartPendingAt');
}

function publicationBlocked(state) {
  return pending(state) || Boolean(state?.reconciliationRequired &&
    (state.automaticGain || state.receipt?.automaticGain));
}

function completedState({state, config, revision, loadedRevision, serverId,
  restartReceipt, processorStatus, timestampReceivedAt, now = Date.now(),
  timestampMaxAgeMs = 10000}) {
  if (!pending(state) || state.reconciliationRequired) return null;
  const gain = state.receipt.automaticGain;
  const since = gain.restartPendingAt;
  const receipt = restartReceipt;
  const processor = processorStatus?.value;
  const recent = (at, maxAgeMs) => Number.isFinite(at) &&
    at <= now && now - at <= maxAgeMs;
  if (!Number.isFinite(since) || since <= 0 || since > now ||
      typeof gain.restartOriginServerId !== 'string' || !gain.restartOriginServerId ||
      typeof serverId !== 'string' || !serverId || gain.restartOriginServerId === serverId ||
      !/^[a-f0-9]{64}$/.test(revision) || loadedRevision !== revision ||
      state.configRevision !== revision || state.receipt.configRevision !== revision ||
      config?.capture?.device?.type !== 'Kraken' || config.capture.replay?.state === true ||
      !receipt || receipt.state !== 'complete' || receipt.inProgress !== false ||
      receipt.configRevision !== revision || !Number.isFinite(receipt.updatedAt) ||
      receipt.updatedAt < since || receipt.updatedAt > now ||
      !recent(processorStatus?.receivedAt, 5000) ||
      processorStatus.receivedAt < receipt.updatedAt ||
      processor?.input !== 'live' || processor.receiver !== 'Kraken' ||
      processor.state !== 'live' || processor.error ||
      processor.frequency !== config.capture.fc || processor.sampleRate !== config.capture.fs ||
      !recent(timestampReceivedAt, timestampMaxAgeMs) || timestampReceivedAt < receipt.updatedAt)
    return null;
  const {restartPendingAt, ...verifiedGain} = gain;
  return {...state, receipt: {...state.receipt, automaticGain: {...verifiedGain,
    restartVerifiedAt: now,
    restartVerification: {serverId, configRevision: revision,
      helperCompletedAt: receipt.updatedAt,
      processorStatusReceivedAt: processorStatus.receivedAt,
      timestampReceivedAt}}}};
}

// Persist before allowing the caller to release its in-memory publication gate.
// A thrown write leaves the original pending state in the caller untouched.
function complete(options, write) {
  const next = completedState(options);
  if (next) write(next);
  return next;
}

module.exports = {pending, publicationBlocked, completedState, complete, installedGainRestartSupported};
