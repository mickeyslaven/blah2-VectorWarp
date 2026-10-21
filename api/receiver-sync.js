'use strict';

const net = require('net');
const {normalizeAdcHeadroom} = require('./kraken-adc-headroom');

const RECEIVER_TYPES = Object.freeze(['Kraken', 'RspDuo', 'Usrp', 'HackRF']);
const MAX_FRAME_BYTES = 65536;
const DEFAULT_STATUS_TIMEOUT_MS = 2200;
const DEFAULT_READBACK_TIMEOUT_MS = 15000;
const RECEIVER_SYNC_BUDGET = Object.freeze({
  defaultTransactionTimeoutMs: 75000,
  maxTransactionTimeoutMs: 75000
});
const RECEIVER_RECALIBRATION_BUDGET = Object.freeze({
  defaultTimeoutMs: 150000,
  maxTimeoutMs: 150000
});

function plainObject(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function cleanMessage(value, fallback) {
  return String(value || fallback).replace(/[\r\n\t]+/g, ' ').slice(0, 240);
}

function receiverError(code, message, status = 502, receipt = null) {
  const error = new Error(message);
  error.code = code;
  error.status = status;
  if (receipt) error.receiverSync = receipt;
  return error;
}

function positiveInteger(value, name) {
  if (!Number.isInteger(value) || value < 1)
    throw receiverError('INVALID_RECEIVER_SYNC', `${name} must be a positive integer.`, 422);
  return value;
}

function endpointFrom(config) {
  const heimdall = config?.capture?.device?.heimdall;
  if (!plainObject(heimdall) || typeof heimdall.host !== 'string' ||
      heimdall.host.length < 1 || heimdall.host.length > 253 || /[\s/@?#]/.test(heimdall.host))
    throw receiverError('INVALID_RECEIVER_SYNC',
      'The Kraken control host is missing or invalid.', 422);
  const controlPort = positiveInteger(heimdall.control_port ?? 8092,
    'capture.device.heimdall.control_port');
  if (controlPort > 65535)
    throw receiverError('INVALID_RECEIVER_SYNC',
      'capture.device.heimdall.control_port must be at most 65535.', 422);
  return {host: heimdall.host, controlPort};
}

function requestedSettings(config) {
  if (config?.capture?.device?.type !== 'Kraken')
    throw receiverError('INVALID_RECEIVER_SYNC', 'Kraken synchronization requires a Kraken profile.', 422);
  const gain = config.capture.device.heimdall?.gain;
  const channelGains = config.capture.device.heimdall?.channel_gains;
  if (gain !== undefined && gain !== 'keep' && gain !== -1 && !(typeof gain === 'number' && Number.isFinite(gain) && gain >= 0 && gain <= 50 && Math.round(gain * 10) === gain * 10))
    throw receiverError('INVALID_RECEIVER_SYNC', 'Kraken gain is invalid.', 422);
  if (channelGains !== undefined && (!Array.isArray(channelGains) ||
      channelGains.length !== config.capture.device.channel_count || channelGains.some(value =>
        !(typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 50 &&
          Math.round(value * 10) === value * 10))))
    throw receiverError('INVALID_RECEIVER_SYNC', 'Kraken per-channel gains are invalid.', 422);
  return {
    frequency: positiveInteger(config.capture.fc, 'capture.fc'),
    sampleRate: positiveInteger(config.capture.fs, 'capture.fs'),
    channelCount: positiveInteger(config.capture.device.channel_count,
      'capture.device.channel_count'),
    ...(gain === undefined || gain === 'keep' ? {} : {gain}),
    ...(channelGains === undefined ? {} : {channelGains: [...channelGains]})
  };
}

function krakenControlValues(config) {
  if (config?.capture?.device?.type !== 'Kraken') return null;
  let endpoint;
  try { endpoint = endpointFrom(config); }
  catch (_) { endpoint = {host: null, controlPort: null}; }
  return {endpoint, iqPort: config.capture.device.heimdall?.port,
    frequency: config.capture.fc, sampleRate: config.capture.fs,
    channelCount: config.capture.device.channel_count,
    gain: config.capture.device.heimdall?.gain === 'keep' ? undefined : config.capture.device.heimdall?.gain,
    channelGains: config.capture.device.heimdall?.channel_gains};
}

function requiresReceiverSynchronization(previous, candidate) {
  if (candidate?.capture?.replay?.state === true ||
      candidate?.capture?.device?.type !== 'Kraken') return false;
  if (previous?.capture?.replay?.state === true) return true;
  const before = krakenControlValues(previous);
  const after = krakenControlValues(candidate);
  return !before || JSON.stringify(before) !== JSON.stringify(after);
}

function normalizeStatus(frame) {
  if (!plainObject(frame) || !plainObject(frame.settings))
    throw receiverError('KRAKEN_PROTOCOL_MISMATCH',
      'Suite V2 did not provide a status object.', 502);
  const centerFrequency = frame.settings.center_freq;
  const sampleRate = frame.settings.sample_rate;
  const channelCount = frame.num_channels;
  const maximumChannels = frame.max_elements;
  const gain = frame.settings.gain;
  const channelGains = frame.channel_gains;
  if (!Number.isFinite(centerFrequency) || !Number.isFinite(sampleRate) ||
      !Number.isInteger(channelCount) || !Number.isInteger(maximumChannels) ||
      channelCount < 1 || maximumChannels < channelCount || maximumChannels > 64 ||
      typeof frame.reconfiguring !== 'boolean' ||
      !['coherent', 'wideband', 'independent'].includes(frame.operating_mode))
    throw receiverError('KRAKEN_PROTOCOL_MISMATCH',
      'Suite V2 status is missing tuning, channel, mode, or reconfiguration fields.', 502);
  if (channelGains !== undefined && (!Array.isArray(channelGains) ||
      channelGains.length !== channelCount || channelGains.some(value =>
        !(Number.isFinite(value) && value >= 0 && value <= 50))))
    throw receiverError('KRAKEN_PROTOCOL_MISMATCH',
      'Suite V2 status contains malformed per-channel gain readback.', 502);
  let adcHeadroom;
  try { adcHeadroom = normalizeAdcHeadroom(frame.adc_headroom, channelCount); }
  catch (error) {
    throw receiverError('KRAKEN_PROTOCOL_MISMATCH', error.message, 502);
  }
  return {
    centerFrequency, sampleRate, channelCount, maximumChannels,
    gain: Number.isFinite(gain) ? gain : null,
    channelGains: channelGains === undefined ? null : [...channelGains],
    operatingMode: frame.operating_mode,
    reconfiguring: frame.reconfiguring,
    recovering: frame.recovering === true,
    cooldownActive: frame.cooldown_active === true,
    adcHeadroom
  };
}

function publicStatus(status) {
  return status && {...status};
}

function validateOptionalCalibrationDetails(frame, requireReady = false) {
  if ((frame.noise_source !== undefined && typeof frame.noise_source !== 'boolean') ||
      (frame.phase_state !== undefined && !Number.isInteger(frame.phase_state)) ||
      (frame.calibration_state !== undefined && !['PENDING', 'CONVERGED', 'FAILED'].includes(frame.calibration_state)))
    throw receiverError('KRAKEN_PROTOCOL_MISMATCH', 'Suite supplied malformed calibration status.', 502);
  if (requireReady && (frame.noise_source === true ||
      (frame.phase_state !== undefined && frame.phase_state !== 4) ||
      (frame.calibration_state !== undefined && frame.calibration_state !== 'CONVERGED')))
    throw receiverError(frame.calibration_state === 'FAILED' ? 'KRAKEN_CALIBRATION_FAILED' :
      'KRAKEN_CALIBRATION_UNVERIFIED', 'Suite reports calibration is not ready for verification.', 409);
}

function validateInitialStatus(status, wanted, {waitForRecovery = false} = {}) {
  if (status.operatingMode !== 'coherent')
    throw receiverError('KRAKEN_MODE_MISMATCH',
      `Suite V2 is in ${status.operatingMode} mode. Live processing requires coherent mode.`, 409);
  if (status.reconfiguring || (status.recovering && !waitForRecovery))
    throw receiverError('KRAKEN_BUSY',
      'Suite V2 is reconfiguring or recovering. Wait until it is idle, then save again.', 409);
  if (status.sampleRate !== wanted.sampleRate)
    throw receiverError('KRAKEN_SAMPLE_RATE_MISMATCH',
      `Suite V2 reports ${status.sampleRate} samples/s; VectorWarp requests ${wanted.sampleRate}. ` +
      'Sample rate is set when Suite starts and cannot be changed here.', 409);
  if (wanted.gain !== undefined && status.gain === null)
    throw receiverError('KRAKEN_GAIN_NOT_REPORTED',
      'Suite V2 did not report gain, so the requested gain cannot be confirmed.', 409);
  if (wanted.channelGains !== undefined && status.channelGains === null)
    throw receiverError('KRAKEN_CHANNEL_GAINS_NOT_REPORTED',
      'Suite V2 does not support coherent per-channel gain readback.', 409);
  // The inspected control contract guarantees this standard tuner range. A
  // wideband/downconverter build can expose more RF, but does not advertise a
  // machine-readable range; do not infer one from an optional status object.
  if (wanted.frequency < 24000000 || wanted.frequency > 1766000000)
    throw receiverError('KRAKEN_FREQUENCY_NOT_AUTOMATABLE',
      'Kraken retuning is limited to the verified 24–1766 MHz control range.', 422);
  if (wanted.channelCount < 2 || wanted.channelCount > 8 ||
      wanted.channelCount > status.maximumChannels)
    throw receiverError('KRAKEN_CHANNEL_COUNT_UNAVAILABLE',
      `Suite V2 has ${status.maximumChannels} configured channels; ` +
      `${wanted.channelCount} requires changing Suite startup configuration.`, 409);
}

function operationList(status, wanted) {
  const operations = [];
  // Retunes and explicit gain changes start a cooldown.  Wait for their
  // calibration to settle before changing the active prefix: element changes
  // start an asynchronous full recovery, so they must be last.
  if (status.centerFrequency !== wanted.frequency) operations.push({
    id: 'set_frequency', field: 'capture.fc', value: wanted.frequency,
    command: {command: 'set_frequency', frequency: wanted.frequency},
    acknowledgementField: 'frequency',
    fullResetAfterAck: status.gain === -1 && (wanted.gain === undefined || wanted.gain === -1),
    matches: observed => observed.centerFrequency === wanted.frequency
  });
  if (wanted.gain !== undefined && status.gain !== wanted.gain) operations.push({
    id: 'set_gain', field: 'capture.device.heimdall.gain', value: wanted.gain,
    command: {command: 'set_gain', gain: wanted.gain}, acknowledgementField: 'gain',
    fullResetAfterAck: wanted.gain === -1 && status.gain !== -1,
    matches: observed => observed.gain === wanted.gain
  });
  if (status.channelCount !== wanted.channelCount) operations.push({
    id: 'set_num_elements', field: 'capture.device.channel_count',
    value: wanted.channelCount,
    command: {command: 'set_num_elements', num_elements: wanted.channelCount},
    acknowledgementField: 'num_elements',
    matches: observed => observed.channelCount === wanted.channelCount &&
      observed.reconfiguring === false
  });
  if (wanted.channelGains !== undefined &&
      JSON.stringify(status.channelGains) !== JSON.stringify(wanted.channelGains)) operations.push({
    id: 'set_channel_gains', field: 'capture.device.heimdall.channel_gains',
    value: [...wanted.channelGains],
    command: {command: 'set_channel_gains', gains: [...wanted.channelGains]},
    acknowledgementField: 'channel_gains',
    matches: observed => JSON.stringify(observed.channelGains) === JSON.stringify(wanted.channelGains)
  });
  return operations;
}

function createReceipt(endpoint, wanted, before) {
  return {
    schemaVersion: 1,
    receiverType: 'Kraken',
    status: 'pending',
    endpoint: {host: endpoint.host, controlPort: endpoint.controlPort},
    requested: {...wanted},
    before: publicStatus(before),
    after: publicStatus(before),
    operations: [],
    acknowledgement: 'Suite command response plus subsequent status readback',
    channelIdentity: {
      preserved: 'Suite channel indexes are never reordered; only the active prefix length is changed.',
      hardwareSerialsVerified: false,
      limitation: 'The Suite TCP status contract does not expose channel serials.'
    },
    calibrationVerified: false,
    hardwareVerified: false
  };
}

function createKrakenControlClient(options = {}) {
  const connector = options.connector || net.createConnection;
  const statusTimeoutMs = options.statusTimeoutMs ?? DEFAULT_STATUS_TIMEOUT_MS;
  const readbackTimeoutMs = options.readbackTimeoutMs ?? DEFAULT_READBACK_TIMEOUT_MS;
  const transactionTimeoutMs = options.transactionTimeoutMs ??
    RECEIVER_SYNC_BUDGET.defaultTransactionTimeoutMs;
  const recalibrationTimeoutMs = options.recalibrationTimeoutMs ??
    RECEIVER_RECALIBRATION_BUDGET.defaultTimeoutMs;
  if (typeof connector !== 'function' || !Number.isInteger(statusTimeoutMs) ||
      statusTimeoutMs < 50 || statusTimeoutMs > 10000 ||
      !Number.isInteger(readbackTimeoutMs) || readbackTimeoutMs < 50 ||
      readbackTimeoutMs > 30000 || !Number.isInteger(transactionTimeoutMs) ||
      transactionTimeoutMs < 100 ||
      transactionTimeoutMs > RECEIVER_SYNC_BUDGET.maxTransactionTimeoutMs ||
      !Number.isInteger(recalibrationTimeoutMs) || recalibrationTimeoutMs < 100 ||
      recalibrationTimeoutMs > RECEIVER_RECALIBRATION_BUDGET.maxTimeoutMs)
    throw receiverError('INVALID_RECEIVER_SYNC', 'Receiver synchronization options are invalid.', 500);

  function transact(config, {readOnly = false, recalibrate = false} = {}) {
    if (recalibrate && config?.capture?.replay?.state === true)
      throw receiverError('INVALID_RECEIVER_SYNC', 'Recalibration requires a live Kraken profile.', 422);
    const endpoint = endpointFrom(config);
    const wanted = requestedSettings(config);
    return new Promise((resolve, reject) => {
      let socket;
      let buffer = '';
      let bufferedBytes = 0;
      let timer;
      let transactionTimer;
      let settled = false;
      let initialStatus = null;
      let lastStatus = null;
      let operations = [];
      let current = null;
      let currentAcknowledged = false;
      let currentRejected = false;
      let currentSent = false;
      let waitingForIdle = false;
      let expectedReadback = null;
      let scannerPending = false;
      let lastFrame = null;
      let resetSent = false, resetAcknowledged = false, resetRejected = false;
      let resetAfterStatus = 0, recoveryAfterStatus = 0;
      let acknowledgedAfterStatus = 0;
      let statusSequence = 0;
      const receipt = createReceipt(endpoint, wanted, null);

      const arm = (milliseconds, message, code = 'KRAKEN_READBACK_TIMEOUT') => {
        clearTimeout(timer);
        timer = setTimeout(() => fail(receiverError(code, message,
          504, failureReceipt())), milliseconds);
      };
      const close = () => {
        clearTimeout(timer);
        clearTimeout(transactionTimer);
        if (socket) socket.destroy();
      };
      const failureReceipt = () => {
        const attempted = current ? [{operation: current.id, field: current.field,
          requested: current.value, commandSent: currentSent,
          acknowledged: currentSent && currentAcknowledged,
          commandOutcome: !currentSent ? 'not-sent' : currentRejected ? 'rejected' :
            currentAcknowledged ? 'acknowledged' : 'unknown',
          readbackMatched: false,
          ...(current.fullResetAfterAck ? {fullRecalibration: {commandSent: resetSent,
            acknowledged: resetAcknowledged, rejected: resetRejected,
            recoveryObserved: recoveryAfterStatus > 0, completed: false}} : {})}] : [];
        const status = receipt.operations.length || currentAcknowledged ? 'partial' :
          currentSent && !currentRejected ? 'indeterminate' : 'failed';
        return {...receipt,
          status,
          after: publicStatus(lastStatus),
          operations: [...receipt.operations, ...attempted]};
      };
      const fail = error => {
        if (settled) return;
        settled = true;
        close();
        if (!error.receiverSync) error.receiverSync = failureReceipt();
        reject(error);
      };
      transactionTimer = setTimeout(() => fail(receiverError('KRAKEN_TRANSACTION_TIMEOUT',
        `Suite V2 ${recalibrate ? 'recalibration' : 'synchronization'} timed out.`, 504,
        failureReceipt())), recalibrate ? recalibrationTimeoutMs : transactionTimeoutMs);
      const finish = () => {
        if (settled) return;
        // Read-only verification is also the final gain-selection check. Stock
        // Suite may omit calibration details, but supplied invalid/non-ready
        // values cannot be silently discarded after a valid earlier IQ probe.
        if (readOnly) {
          try { validateOptionalCalibrationDetails(lastFrame, true); }
          catch (error) { return fail(error); }
        }
        if (!matchesTuple(lastStatus, wanted.frequency, wanted.channelCount, wanted.gain,
            wanted.channelGains))
          return fail(receiverError('KRAKEN_STATUS_DRIFT',
            'Suite V2 no longer reports the requested settings. The config was not saved.', 409));
        settled = true;
        close();
        receipt.status = recalibrate ? 'recalibrated' :
          receipt.operations.length ? 'synchronized' : 'already-matched';
        receipt.after = publicStatus(lastStatus);
        resolve(receipt);
      };
      const suiteIdle = observed => observed && !observed.reconfiguring &&
        !observed.recovering && !observed.cooldownActive;
      const validateRecoveryStatus = (frame, observed) => {
        if (observed.operatingMode !== 'coherent' || observed.sampleRate !== wanted.sampleRate ||
            observed.channelCount !== initialStatus.channelCount || observed.reconfiguring ||
            (recalibrate && (observed.centerFrequency !== wanted.frequency ||
              observed.channelCount !== wanted.channelCount ||
              observed.gain !== (wanted.gain ?? initialStatus.gain))))
          throw receiverError('KRAKEN_STATUS_DRIFT', 'Suite mode, sample rate or channel count changed during planned AUTO recovery.', 409);
        if (frame.kerberos_mode === true || frame.external_noise === true)
          throw receiverError('KRAKEN_RESET_UNSUPPORTED', 'Automatic recovery requires the normal internal-noise Kraken workflow.', 409);
        if (typeof frame.recovering !== 'boolean' || typeof frame.cooldown_active !== 'boolean' ||
            (frame.kerberos_mode !== undefined && typeof frame.kerberos_mode !== 'boolean') ||
            (frame.external_noise !== undefined && typeof frame.external_noise !== 'boolean'))
          throw receiverError('KRAKEN_PROTOCOL_MISMATCH', 'Suite omitted or malformed required AUTO recovery status.', 502);
        validateOptionalCalibrationDetails(frame);
      };
      const sendFullRecalibration = () => {
        validateRecoveryStatus(lastFrame, lastStatus);
        resetSent = true;
        resetAfterStatus = statusSequence;
        socket.write(`${JSON.stringify({command: 'reset_lag_compensation'})}\n`);
      };
      const sendCurrent = () => {
        if (current.fullResetAfterAck) validateRecoveryStatus(lastFrame, lastStatus);
        expectedReadback = {frequency: lastStatus.centerFrequency,
          channelCount: lastStatus.channelCount,
          gain: wanted.gain === undefined && !current.fullResetAfterAck ? undefined : lastStatus.gain,
          channelGains: lastStatus.channelGains};
        if (current.id === 'set_frequency') expectedReadback.frequency = current.value;
        if (current.id === 'set_gain') expectedReadback.gain = current.value;
        if (current.id === 'set_num_elements') expectedReadback.channelCount = current.value;
        if (current.id === 'set_channel_gains') expectedReadback.channelGains = current.value;
        // A delayed response from an earlier command must not authorize this
        // operation. Only the exact write below establishes its ACK window.
        currentAcknowledged = false;
        currentRejected = false;
        currentSent = true;
        acknowledgedAfterStatus = statusSequence;
        arm(recalibrate ? recalibrationTimeoutMs : readbackTimeoutMs,
          `Suite V2 did not confirm ${current.field}=${current.value} before the deadline.`);
        if (current.resetOnly) sendFullRecalibration();
        else socket.write(`${JSON.stringify(current.command)}\n`);
      };
      const next = () => {
        current = operations.shift() || null;
        currentAcknowledged = false;
        currentRejected = false;
        currentSent = false;
        expectedReadback = null;
        resetSent = resetAcknowledged = resetRejected = false;
        resetAfterStatus = recoveryAfterStatus = 0;
        if (!current) return finish();
        if (!suiteIdle(lastStatus)) {
          waitingForIdle = true;
          arm(recalibrate ? recalibrationTimeoutMs : readbackTimeoutMs,
            `Suite V2 did not become idle before ${current.field}=${current.value}.`);
          return;
        }
        sendCurrent();
      };
      const acceptReadback = () => {
        // Stock Suite reports the recovery cycle but omits these optional
        // calibration details. Preserve unknowns; only a fresh coherent MCHQ
        // capture can supply the missing stream evidence to a gain coordinator.
        const missingCalibrationDetails = recalibrate &&
          ['phase_state', 'calibration_state', 'noise_source'].some(key => lastFrame[key] === undefined);
        receipt.operations.push({operation: current.id, field: current.field,
          requested: current.value, commandSent: true, commandOutcome: 'acknowledged',
          acknowledged: true, readbackMatched: true,
          ...(current.fullResetAfterAck ? {fullRecalibration: {commandSent: true,
            acknowledged: true, recoveryObserved: true, completed: true,
            verification: missingCalibrationDetails ?
              'reported recovery completion; requires fresh coherent MCHQ validation; no RF measurement' :
              'reported recovery completion; no RF measurement',
            ...(recalibrate ? {phaseState: lastFrame.phase_state ?? null,
              calibrationState: lastFrame.calibration_state ?? null, noiseSource: lastFrame.noise_source ?? null} : {})}} : {})});
        current = null;
        currentAcknowledged = false;
        next();
      };
      const matchesTuple = (observed, frequency, count, gain, channelGains) => observed &&
        observed.operatingMode === 'coherent' && observed.sampleRate === wanted.sampleRate &&
        observed.centerFrequency === frequency && observed.channelCount === count &&
        (gain === undefined || observed.gain === gain) &&
        (channelGains === undefined ||
          JSON.stringify(observed.channelGains) === JSON.stringify(channelGains)) &&
        observed.reconfiguring === false;
      const handleStatus = frame => {
        const observed = normalizeStatus(frame);
        statusSequence += 1;
        lastStatus = observed;
        lastFrame = frame;
        if (!initialStatus) {
          initialStatus = observed;
          receipt.before = publicStatus(observed);
          receipt.after = publicStatus(observed);
          try { validateInitialStatus(observed, wanted, {waitForRecovery: recalibrate}); }
          catch (error) { return fail(error); }
          if (recalibrate) {
            if (observed.gain === null)
              return fail(receiverError('KRAKEN_GAIN_NOT_REPORTED',
                'Suite V2 must report gain before a full recalibration.', 409));
            validateRecoveryStatus(frame, observed);
            operations = [{id: 'reset_lag_compensation', field: 'calibration',
              value: 'lag-and-phase', fullResetAfterAck: true, resetOnly: true,
              matches: status => matchesTuple(status, wanted.frequency, wanted.channelCount,
                wanted.gain ?? initialStatus.gain, wanted.channelGains)}];
          } else operations = operationList(observed, wanted);
          if (readOnly && operations.length)
            return fail(receiverError('KRAKEN_STARTUP_MISMATCH',
              'Suite settings differ from saved receiver settings. Apply them before starting live processing.', 409));
          if (operations.some(operation => operation.fullResetAfterAck)) {
            validateRecoveryStatus(frame, observed);
            scannerPending = true;
            arm(statusTimeoutMs, 'Suite V2 did not report scanner state before planned recovery.', 'KRAKEN_STATUS_TIMEOUT');
            socket.write(`${JSON.stringify({command: 'get_scanner_status'})}\n`);
            return;
          }
          return next();
        }
        if (recalibrate) validateRecoveryStatus(frame, observed);
        if (scannerPending) return;
        if (current?.fullResetAfterAck && resetSent) {
          validateRecoveryStatus(frame, observed);
          if (resetAcknowledged && statusSequence > acknowledgedAfterStatus && observed.recovering && current.matches(observed) &&
              matchesTuple(observed, expectedReadback.frequency, expectedReadback.channelCount,
                expectedReadback.gain, expectedReadback.channelGains))
            recoveryAfterStatus ||= statusSequence;
          if ((recoveryAfterStatus || (recalibrate && resetAcknowledged)) && frame.calibration_state === 'FAILED')
            return fail(receiverError('KRAKEN_CALIBRATION_FAILED', 'Suite full recalibration reported FAILED. The config was not saved.', 409));
        }
        if (waitingForIdle) {
          if (!suiteIdle(observed)) return;
          waitingForIdle = false;
          return sendCurrent();
        }
        if (current && currentAcknowledged && statusSequence > acknowledgedAfterStatus &&
            (!current.fullResetAfterAck || (resetAcknowledged && recoveryAfterStatus > 0 &&
              statusSequence > recoveryAfterStatus && suiteIdle(observed) && frame.noise_source !== true &&
              (frame.phase_state === undefined || frame.phase_state === 4) &&
              (frame.calibration_state === undefined || frame.calibration_state === 'CONVERGED'))) &&
            current.matches(observed) && matchesTuple(observed, expectedReadback.frequency,
              expectedReadback.channelCount, expectedReadback.gain, expectedReadback.channelGains))
          acceptReadback();
      };
      const handleResponse = frame => {
        if (scannerPending) {
          if (frame.status !== 'success' || frame.enabled !== false)
            return fail(receiverError('KRAKEN_RESET_UNSUPPORTED', 'Planned AUTO recovery requires an explicitly disabled scanner.', 409));
          scannerPending = false;
          receipt.scannerPreflight = {enabled: false, atomicOwnershipGuaranteed: false};
          return next();
        }
        if (current?.fullResetAfterAck && resetSent) {
          if (frame.status !== 'success') {
            resetRejected = true;
            if (current.resetOnly) currentRejected = true;
            return fail(receiverError('KRAKEN_COMMAND_REJECTED', 'Suite rejected the planned full recalibration.', 409));
          }
          if (resetAcknowledged || frame.message !== 'Full recalibration (lag + phase) started')
            return fail(receiverError('KRAKEN_ACKNOWLEDGEMENT_MISMATCH', 'Suite did not acknowledge the planned full recalibration.', 502));
          resetAcknowledged = true;
          if (current.resetOnly) currentAcknowledged = true;
          acknowledgedAfterStatus = statusSequence;
          return;
        }
        if (!current || !currentSent) return fail(receiverError('KRAKEN_UNEXPECTED_RESPONSE',
          'Suite V2 responded before a command was sent.', 502));
        if (frame.status !== 'success') {
          currentRejected = true;
          return fail(receiverError('KRAKEN_COMMAND_REJECTED',
            `Suite V2 rejected ${current.field}: ${cleanMessage(frame.message, 'unknown error')}`,
            409, failureReceipt()));
        }
        const acknowledgedValue = frame[current.acknowledgementField];
        // The inspected Suite parses gain as float32 and serializes its ACK
        // with std::to_string (six decimal places): 49.6 becomes 49.599998.
        // Accept only that exact wire encoding or an exact-value implementation;
        // do not loosen subsequent full-tuple/tenth-dB status readback.
        const wireValue = current.id === 'set_gain' ?
          Number(Math.fround(current.value).toFixed(6)) : current.value;
        const acknowledgementMatches = current.id === 'set_channel_gains' ?
          JSON.stringify(acknowledgedValue) === JSON.stringify(current.value) :
          acknowledgedValue === current.value || acknowledgedValue === wireValue;
        if (!acknowledgementMatches)
          return fail(receiverError('KRAKEN_ACKNOWLEDGEMENT_MISMATCH',
            `Suite V2 confirmed an unexpected ${current.field} value.`, 502,
            failureReceipt()));
        currentAcknowledged = true;
        // A status received before this exact ACK is never its readback.
        acknowledgedAfterStatus = statusSequence;
        if (current.fullResetAfterAck) {
          sendFullRecalibration();
        }
      };
      const handleLine = line => {
        if (!line.trim()) return;
        if (Buffer.byteLength(line) > MAX_FRAME_BYTES)
          return fail(receiverError('KRAKEN_PROTOCOL_MISMATCH',
            'Suite V2 returned an oversized control frame.', 502));
        let frame;
        try { frame = JSON.parse(line); }
        catch (_) { return fail(receiverError('KRAKEN_PROTOCOL_MISMATCH',
          'Suite V2 returned malformed control data.', 502)); }
        try {
          if (plainObject(frame.settings)) return handleStatus(frame);
          if (typeof frame.status === 'string') return handleResponse(frame);
          fail(receiverError('KRAKEN_PROTOCOL_MISMATCH',
            'Suite V2 returned an unrecognized control response.', 502));
        } catch (error) {
          fail(error?.code ? error : receiverError('KRAKEN_PROTOCOL_MISMATCH',
            'Suite V2 returned an invalid control response.', 502));
        }
      };

      try { socket = connector({host: endpoint.host, port: endpoint.controlPort}); }
      catch (error) {
        fail(receiverError('KRAKEN_CONNECTION_FAILED',
          `Could not connect to Suite V2: ${cleanMessage(error.message, 'connection failed')}`,
          502));
        return;
      }
      socket.setEncoding('utf8');
      if (typeof socket.setNoDelay === 'function') socket.setNoDelay(true);
      arm(statusTimeoutMs,
        'Suite V2 did not send a compatible status update before the deadline.',
        'KRAKEN_STATUS_TIMEOUT');
      socket.on('data', chunk => {
        bufferedBytes += Buffer.byteLength(chunk);
        if (bufferedBytes > MAX_FRAME_BYTES * 4)
          return fail(receiverError('KRAKEN_PROTOCOL_MISMATCH',
            'Suite V2 control traffic exceeded the transaction limit.', 502));
        buffer += chunk;
        const lines = buffer.split('\n');
        buffer = lines.pop();
        for (const line of lines) {
          if (settled) return;
          handleLine(line);
        }
      });
      socket.on('error', error => fail(receiverError('KRAKEN_CONNECTION_FAILED',
        `Suite V2 control connection failed: ${cleanMessage(error.message, 'connection failed')}`,
        502)));
      socket.on('close', () => {
        if (!settled) fail(receiverError('KRAKEN_CONNECTION_CLOSED',
          'Suite V2 closed the control connection before confirming the change.', 502));
      });
    });
  }

  return {synchronize: (config, options) => transact(config, options),
    verify: config => transact(config, {readOnly: true}),
    recalibrate: config => transact(config, {recalibrate: true})};
}

function receiverAcceptanceBoundary(config) {
  const receiverType = config?.capture?.device?.type || 'Unknown';
  const replay = config?.capture?.replay?.state === true;
  if (replay) return {receiverType, mode: 'replay', configurationAuthority: 'VectorWarp',
    writePath: 'none', acknowledgement: 'recording parser and radar status',
    hardwareReadback: false, physicalReceiverVerified: false};
  if (receiverType === 'Kraken') return {receiverType, mode: 'live',
    configurationAuthority: {
      upstream: ['capture.fc', 'capture.fs', 'capture.device.channel_count', 'Suite gain and hardware serial order'],
      vectorwarp: ['capture.device.heimdall', 'capture.device.reference_channel',
        'capture.device.surveillance_channels', 'process.reference_synthesis']
    }, writePath: 'Suite V2 TCP control for frequency, gain and active element count, with conditional existing full recalibration',
    acknowledgement: 'command response plus a subsequent Suite status broadcast',
    upstreamReadback: true,
    hardwareReadback: false, physicalReceiverVerified: false};
  if (receiverType === 'RspDuo') return {receiverType, mode: 'live',
    configurationAuthority: 'VectorWarp startup configuration',
    writePath: 'processor startup through SDRplay API v3',
    acknowledgement: 'SDK return codes are checked; failures appear in radar status',
    positiveAppliedAcknowledgement: false,
    readbackBoundary: 'Both tuner parameter records are set explicitly; post-init independent tuner-value readback is unavailable.',
    hardwareReadback: false, physicalReceiverVerified: false};
  if (receiverType === 'Usrp') return {receiverType, mode: 'live',
    configurationAuthority: 'VectorWarp startup configuration',
    writePath: 'processor startup through UHD multi_usrp',
    acknowledgement: 'UHD exceptions can report failure; no positive applied-settings acknowledgement is emitted',
    positiveAppliedAcknowledgement: false,
    readbackBoundary: 'UHD startup getters check frequency, rate, gain, antenna, and subdevice mapping before streaming; radar status does not confirm applied values.',
    hardwareReadback: false, physicalReceiverVerified: false};
  if (receiverType === 'HackRF') return {receiverType, mode: 'live',
    configurationAuthority: 'VectorWarp startup configuration',
    writePath: 'processor startup through two serial-selected libhackrf devices',
    acknowledgement: 'Every current open/set/start return code is checked and can report failure',
    positiveAppliedAcknowledgement: false,
    readbackBoundary: 'No post-set frequency, rate, gain, amplifier, clock, or synchronization readback is implemented.',
    hardwareReadback: false, physicalReceiverVerified: false};
  return {receiverType, mode: 'unknown', configurationAuthority: 'none',
    writePath: 'unsupported', acknowledgement: 'none', hardwareReadback: false,
    physicalReceiverVerified: false};
}

function createReceiverSynchronizer(options = {}) {
  const kraken = options.kraken || createKrakenControlClient(options.krakenOptions);
  return {
    requires: requiresReceiverSynchronization,
    boundary: receiverAcceptanceBoundary,
    async verify(config) {
      if (config?.capture?.device?.type !== 'Kraken' || config?.capture?.replay?.state === true)
        throw receiverError('INVALID_RECEIVER_SYNC', 'Verification requires a live Kraken profile.', 422);
      const result = await kraken.verify(config);
      return {...result, acceptance: receiverAcceptanceBoundary(config)};
    },
    async recalibrate(config) {
      if (config?.capture?.device?.type !== 'Kraken' || config?.capture?.replay?.state === true)
        throw receiverError('INVALID_RECEIVER_SYNC', 'Recalibration requires a live Kraken profile.', 422);
      const result = await kraken.recalibrate(config);
      return {...result, acceptance: receiverAcceptanceBoundary(config)};
    },
    async synchronize(previous, candidate, {force = false} = {}) {
      const liveKraken = candidate?.capture?.device?.type === 'Kraken' &&
        candidate?.capture?.replay?.state !== true;
      if ((!force || !liveKraken) && !requiresReceiverSynchronization(previous, candidate)) return {
        schemaVersion: 1, receiverType: candidate?.capture?.device?.type || 'Unknown',
        status: 'not-required', operations: [],
        acceptance: receiverAcceptanceBoundary(candidate)
      };
      const result = await kraken.synchronize(candidate);
      return {...result, acceptance: receiverAcceptanceBoundary(candidate)};
    }
  };
}

module.exports = {createKrakenControlClient, createReceiverSynchronizer,
  endpointFrom, normalizeStatus, receiverAcceptanceBoundary, RECEIVER_SYNC_BUDGET,
  RECEIVER_RECALIBRATION_BUDGET,
  requiresReceiverSynchronization};
