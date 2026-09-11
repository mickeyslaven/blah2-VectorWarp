'use strict';

const RECEIVERS = new Set(['Kraken', 'RspDuo', 'Usrp', 'HackRF']);
const STATES = new Set(['loading', 'playing', 'draining', 'complete', 'stopped', 'error', 'live']);
const MAX_TEXT = 4096;
const MAX_ERROR_TEXT = 512;
const MAX_SAFE = Number.MAX_SAFE_INTEGER;

function text(value, name, errors, required = true) {
  if (value === undefined && !required) return '';
  if (typeof value !== 'string' || value.length > MAX_TEXT || /[\u0000-\u001f]/.test(value)) {
    errors.push(`${name} must be a short printable string`);
    return '';
  }
  return value;
}
function count(value, name, errors) {
  if (!Number.isSafeInteger(value) || value < 0 || value > MAX_SAFE)
    errors.push(`${name} must be a non-negative safe integer`);
  return Number.isSafeInteger(value) && value >= 0 ? value : 0;
}
function errorText(value, name, errors) {
  if (typeof value !== 'string') {
    errors.push(`${name} must be a string`);
    return '';
  }
  // Exception text can contain newlines. Preserve its meaning without allowing
  // unbounded/control text into status labels or logs.
  return value.replace(/[\u0000-\u001f\u007f]+/g, ' ').replace(/\s+/g, ' ')
    .trim().slice(0, MAX_ERROR_TEXT);
}
function positive32(value, name, errors) {
  if (!Number.isSafeInteger(value) || value < 1 || value > 4294967295)
    errors.push(`${name} must be a positive 32-bit integer`);
  return Number.isSafeInteger(value) && value > 0 && value <= 4294967295 ? value : 0;
}
function status(payload) {
  const errors = [];
  if (!payload || typeof payload !== 'object' || Array.isArray(payload))
    return {valid: false, errors: ['status must be an object']};
  const input = text(payload.input, 'input', errors);
  const receiver = text(payload.receiver, 'receiver', errors);
  const state = text(payload.state, 'state', errors);
  if (!['live', 'replay'].includes(input)) errors.push('input is invalid');
  if (!RECEIVERS.has(receiver)) errors.push('receiver is invalid');
  if (!STATES.has(state)) errors.push('state is invalid');
  const value = {
    input, receiver, state,
    error: errorText(payload.error, 'error', errors),
    file: text(payload.file, 'file', errors),
    sampleRate: positive32(payload.sampleRate, 'sampleRate', errors),
    frequency: positive32(payload.frequency, 'frequency', errors),
    positionSamples: count(payload.positionSamples, 'positionSamples', errors),
    totalSamples: count(payload.totalSamples, 'totalSamples', errors),
    loops: count(payload.loops, 'loops', errors),
    trailingSamples: count(payload.trailingSamples, 'trailingSamples', errors),
    recording: payload.recording === true,
    recordingFile: text(payload.recordingFile, 'recordingFile', errors),
    recordingError: errorText(payload.recordingError, 'recordingError', errors),
    recordedSamples: count(payload.recordedSamples, 'recordedSamples', errors),
    recordingRequestId: payload.recordingRequestId === undefined ? null : count(payload.recordingRequestId, 'recordingRequestId', errors)
  };
  if (typeof payload.recording !== 'boolean') errors.push('recording must be a boolean');
  if (value.input === 'live' && value.state !== 'live' && value.state !== 'error' && value.state !== 'stopped')
    errors.push('live input has an invalid state');
  if (value.input === 'replay' && value.recording) errors.push('recording is unavailable during replay');
  if (value.state === 'error' && !value.error) errors.push('error state requires error text');
  if (value.positionSamples > value.totalSamples && value.totalSamples > 0)
    errors.push('positionSamples exceeds totalSamples');
  return errors.length ? {valid: false, errors} : {valid: true, value};
}

function fresh(entry, now = Date.now(), maxAgeMs = 5000) {
  return !!entry && Number.isFinite(entry.receivedAt) && now - entry.receivedAt <= maxAgeMs;
}
module.exports = {status, fresh, MAX_TEXT};
