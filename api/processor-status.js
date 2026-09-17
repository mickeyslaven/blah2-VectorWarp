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
function positive32(value, name, errors, failed = false) {
  // A failed native startup may be reporting the invalid zero from a hand-edited
  // config. Preserve its error instead of rejecting the entire status update.
  const minimum = failed ? 0 : 1;
  if (!Number.isSafeInteger(value) || value < minimum || value > 4294967295)
    errors.push(`${name} must be a ${failed ? 'non-negative' : 'positive'} 32-bit integer`);
  return Number.isSafeInteger(value) && value >= minimum && value <= 4294967295 ? value : 0;
}
const RSP_STAGES = ['open', 'apiVersion', 'lock', 'enumerate', 'select',
  'unlock', 'debugEnable', 'getDeviceParams', 'init', 'gainUpdateA', 'gainUpdateB'];
function rspStartup(payload, outer, errors) {
  const name = 'receiverStartup';
  if (outer.input !== 'live' || outer.receiver !== 'RspDuo' ||
      !payload || typeof payload !== 'object' || Array.isArray(payload)) {
    errors.push(`${name} requires a live RspDuo object`); return null;
  }
  const requested = payload.requested;
  const selected = payload.selected;
  const sdk = payload.sdk;
  const stages = sdk?.stages;
  if (payload.schema !== 1 || payload.receiver !== 'RspDuo' ||
      !['pending', 'accepted'].includes(payload.status) ||
      payload.hardwareVerified !== false || payload.readbackAvailable !== false ||
      !requested || typeof requested !== 'object' || Array.isArray(requested) ||
      !sdk || typeof sdk !== 'object' || Array.isArray(sdk) ||
      !stages || typeof stages !== 'object' || Array.isArray(stages)) {
    errors.push(`${name} has an invalid schema`); return null;
  }
  const serial = text(requested.serial, `${name}.requested.serial`, errors);
  const frequency = positive32(requested.frequency, `${name}.requested.frequency`, errors);
  const sampleRate = positive32(requested.sampleRate, `${name}.requested.sampleRate`, errors);
  const integer = (value, field, min, max) => {
    if (!Number.isInteger(value) || value < min || value > max)
      errors.push(`${name}.${field} is invalid`);
    return value;
  };
  const agcSetPoint = integer(requested.agcSetPoint, 'requested.agcSetPoint', -72, 0);
  const bandwidthNumber = integer(requested.bandwidthNumber, 'requested.bandwidthNumber', 0, 100);
  if (![0, 5, 50, 100].includes(bandwidthNumber)) errors.push(`${name}.requested.bandwidthNumber is unsupported`);
  const gainReduction = requested.gainReduction;
  if (!Array.isArray(gainReduction) || gainReduction.length !== 2) errors.push(`${name}.requested.gainReduction requires two values`);
  else gainReduction.forEach((gain, index) => integer(gain, `requested.gainReduction[${index}]`, 20, 59));
  const lnaState = integer(requested.lnaState, 'requested.lnaState', 0, 9);
  const maxLna = frequency < 60000000 ? 6 : frequency < 1000000000 ? 9 : 8;
  if (lnaState > maxLna) errors.push(`${name}.requested.lnaState exceeds the band limit`);
  for (const field of ['dabNotch', 'rfNotch'])
    if (typeof requested[field] !== 'boolean') errors.push(`${name}.requested.${field} must be boolean`);
  const ifBandwidthKhz = integer(requested.ifBandwidthKhz, 'requested.ifBandwidthKhz', 1, 10000);
  const ifFrequencyKhz = integer(requested.ifFrequencyKhz, 'requested.ifFrequencyKhz', 0, 10000);
  const decimation = integer(requested.decimation, 'requested.decimation', 1, 32);
  if (![1, 2, 4, 8, 16, 32].includes(decimation)) errors.push(`${name}.requested.decimation is unsupported`);
  const outputModes = new Map([[2000000, [1, 1536]], [1000000, [2, 600]],
    [500000, [4, 300]], [250000, [8, 200]], [125000, [16, 200]],
    [62500, [32, 200]]]);
  const mode = outputModes.get(sampleRate);
  if (!mode || mode[0] !== decimation || mode[1] !== ifBandwidthKhz || ifFrequencyKhz !== 1620)
    errors.push(`${name}.requested IF and decimation do not match output rate`);
  if (frequency !== outer.frequency || sampleRate !== outer.sampleRate)
    errors.push(`${name}.requested frequency and sample rate must match processor status`);
  if (selected !== null && (!selected || typeof selected !== 'object' || Array.isArray(selected)))
    errors.push(`${name}.selected must be an object or null`);
  if (selected && typeof selected === 'object' && !Array.isArray(selected)) {
    const selectedSerial = text(selected.serial, `${name}.selected.serial`, errors);
    if (!selectedSerial || (serial && serial !== selectedSerial)) errors.push(`${name}.selected.serial does not match request`);
    count(selected.deviceIndex, `${name}.selected.deviceIndex`, errors);
    count(selected.hardwareVersion, `${name}.selected.hardwareVersion`, errors);
    if (selected.tuner !== 'Both' || selected.mode !== 'Dual_Tuner')
      errors.push(`${name}.selected has an unsupported mode`);
  }
  for (const stage of RSP_STAGES)
    if (typeof stages[stage] !== 'boolean') errors.push(`${name}.sdk.stages.${stage} must be boolean`);
  if (stages.select !== (selected !== null)) errors.push(`${name}.selected and select stage disagree`);
  if (sdk.version !== null && (typeof sdk.version !== 'number' ||
      !Number.isFinite(sdk.version) || sdk.version <= 0 || sdk.version > 100))
    errors.push(`${name}.sdk.version is invalid`);
  if (stages.apiVersion && sdk.version === null)
    errors.push(`${name}.sdk.version is required after the version check`);
  const allAccepted = RSP_STAGES.every(stage => stages[stage] === true);
  if ((payload.status === 'accepted') !== allAccepted)
    errors.push(`${name}.status disagrees with SDK stages`);
  if (payload.status === 'accepted' && !selected)
    errors.push(`${name}.accepted requires a selected receiver`);
  return {schema: 1, receiver: 'RspDuo', status: payload.status,
    hardwareVerified: false, readbackAvailable: false,
    requested: {serial, frequency, sampleRate, agcSetPoint, bandwidthNumber,
      gainReduction, lnaState, dabNotch: requested.dabNotch, rfNotch: requested.rfNotch,
      ifBandwidthKhz, ifFrequencyKhz, decimation},
    selected: selected ? {serial: selected.serial, deviceIndex: selected.deviceIndex,
      hardwareVersion: selected.hardwareVersion, tuner: 'Both', mode: 'Dual_Tuner'} : null,
    sdk: {version: sdk.version, stages: Object.fromEntries(RSP_STAGES.map(stage => [stage, stages[stage]]))}};
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
    sampleRate: positive32(payload.sampleRate, 'sampleRate', errors, state === 'error'),
    frequency: positive32(payload.frequency, 'frequency', errors, state === 'error'),
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
  if (payload.receiverStartup !== undefined)
    value.receiverStartup = rspStartup(payload.receiverStartup, value, errors);
  return errors.length ? {valid: false, errors} : {valid: true, value};
}

function fresh(entry, now = Date.now(), maxAgeMs = 5000) {
  return !!entry && Number.isFinite(entry.receivedAt) && now - entry.receivedAt <= maxAgeMs;
}
module.exports = {status, fresh, MAX_TEXT};
