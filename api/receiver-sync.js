'use strict';

const net = require('net');

const RECEIVER_TYPES = Object.freeze(['Kraken', 'RspDuo', 'Usrp', 'HackRF']);
const MAX_FRAME_BYTES = 65536;
const DEFAULT_STATUS_TIMEOUT_MS = 2200;
const DEFAULT_READBACK_TIMEOUT_MS = 15000;
const RECEIVER_SYNC_BUDGET = Object.freeze({
  defaultTransactionTimeoutMs: 75000,
  maxTransactionTimeoutMs: 75000
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
  if (gain !== undefined && gain !== 'keep' && gain !== -1 && !(typeof gain === 'number' && Number.isFinite(gain) && gain >= 0 && gain <= 50 && Math.round(gain * 10) === gain * 10))
    throw receiverError('INVALID_RECEIVER_SYNC', 'Kraken gain is invalid.', 422);
  return {
    frequency: positiveInteger(config.capture.fc, 'capture.fc'),
    sampleRate: positiveInteger(config.capture.fs, 'capture.fs'),
    channelCount: positiveInteger(config.capture.device.channel_count,
      'capture.device.channel_count'),
    ...(gain === undefined || gain === 'keep' ? {} : {gain})
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
    gain: config.capture.device.heimdall?.gain === 'keep' ? undefined : config.capture.device.heimdall?.gain};
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
  if (!Number.isFinite(centerFrequency) || !Number.isFinite(sampleRate) ||
      !Number.isInteger(channelCount) || !Number.isInteger(maximumChannels) ||
      channelCount < 1 || maximumChannels < channelCount || maximumChannels > 64 ||
      typeof frame.reconfiguring !== 'boolean' ||
      !['coherent', 'wideband'].includes(frame.operating_mode))
    throw receiverError('KRAKEN_PROTOCOL_MISMATCH',
      'Suite V2 status is missing tuning, channel, mode, or reconfiguration fields.', 502);
  return {
    centerFrequency, sampleRate, channelCount, maximumChannels,
    gain: Number.isFinite(gain) ? gain : null,
    operatingMode: frame.operating_mode,
    reconfiguring: frame.reconfiguring,
    recovering: frame.recovering === true,
    cooldownActive: frame.cooldown_active === true
  };
}

function publicStatus(status) {
  return status && {...status};
}

function validateInitialStatus(status, wanted) {
  if (status.operatingMode !== 'coherent')
    throw receiverError('KRAKEN_MODE_MISMATCH',
      `Suite V2 is in ${status.operatingMode} mode. Live processing requires coherent mode.`, 409);
  if (status.reconfiguring || status.recovering)
    throw receiverError('KRAKEN_BUSY',
      'Suite V2 is reconfiguring or recovering. Wait until it is idle, then save again.', 409);
  if (status.sampleRate !== wanted.sampleRate)
    throw receiverError('KRAKEN_SAMPLE_RATE_MISMATCH',
      `Suite V2 reports ${status.sampleRate} samples/s; VectorWarp requests ${wanted.sampleRate}. ` +
      'Sample rate is set when Suite starts and cannot be changed here.', 409);
  if (wanted.gain !== undefined && status.gain === null)
    throw receiverError('KRAKEN_GAIN_NOT_REPORTED',
      'Suite V2 did not report gain, so the requested gain cannot be confirmed.', 409);
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
    matches: observed => observed.centerFrequency === wanted.frequency
  });
  if (wanted.gain !== undefined && status.gain !== wanted.gain) operations.push({
    id: 'set_gain', field: 'capture.device.heimdall.gain', value: wanted.gain,
    command: {command: 'set_gain', gain: wanted.gain}, acknowledgementField: 'gain',
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
  if (typeof connector !== 'function' || !Number.isInteger(statusTimeoutMs) ||
      statusTimeoutMs < 50 || statusTimeoutMs > 10000 ||
      !Number.isInteger(readbackTimeoutMs) || readbackTimeoutMs < 50 ||
      readbackTimeoutMs > 30000 || !Number.isInteger(transactionTimeoutMs) ||
      transactionTimeoutMs < 100 ||
      transactionTimeoutMs > RECEIVER_SYNC_BUDGET.maxTransactionTimeoutMs)
    throw receiverError('INVALID_RECEIVER_SYNC', 'Receiver synchronization options are invalid.', 500);

  function synchronize(config, {readOnly = false} = {}) {
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
          readbackMatched: false}] : [];
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
        'Suite V2 synchronization timed out.', 504,
        failureReceipt())), transactionTimeoutMs);
      const finish = () => {
        if (settled) return;
        if (!matchesTuple(lastStatus, wanted.frequency, wanted.channelCount, wanted.gain))
          return fail(receiverError('KRAKEN_STATUS_DRIFT',
            'Suite V2 no longer reports the requested settings. The config was not saved.', 409));
        settled = true;
        close();
        receipt.status = receipt.operations.length ? 'synchronized' : 'already-matched';
        receipt.after = publicStatus(lastStatus);
        resolve(receipt);
      };
      const suiteIdle = observed => observed && !observed.reconfiguring &&
        !observed.recovering && !observed.cooldownActive;
      const sendCurrent = () => {
        expectedReadback = {frequency: lastStatus.centerFrequency,
          channelCount: lastStatus.channelCount,
          gain: wanted.gain === undefined ? undefined : lastStatus.gain};
        if (current.id === 'set_frequency') expectedReadback.frequency = current.value;
        if (current.id === 'set_gain') expectedReadback.gain = current.value;
        if (current.id === 'set_num_elements') expectedReadback.channelCount = current.value;
        // A delayed response from an earlier command must not authorize this
        // operation. Only the exact write below establishes its ACK window.
        currentAcknowledged = false;
        currentRejected = false;
        currentSent = true;
        acknowledgedAfterStatus = statusSequence;
        arm(readbackTimeoutMs,
          `Suite V2 did not confirm ${current.field}=${current.value} before the deadline.`);
        socket.write(`${JSON.stringify(current.command)}\n`);
      };
      const next = () => {
        current = operations.shift() || null;
        currentAcknowledged = false;
        currentRejected = false;
        currentSent = false;
        expectedReadback = null;
        if (!current) return finish();
        if (!suiteIdle(lastStatus)) {
          waitingForIdle = true;
          arm(readbackTimeoutMs,
            `Suite V2 did not become idle before ${current.field}=${current.value}.`);
          return;
        }
        sendCurrent();
      };
      const acceptReadback = () => {
        receipt.operations.push({operation: current.id, field: current.field,
          requested: current.value, commandSent: true, commandOutcome: 'acknowledged',
          acknowledged: true, readbackMatched: true});
        current = null;
        currentAcknowledged = false;
        next();
      };
      const matchesTuple = (observed, frequency, count, gain) => observed &&
        observed.operatingMode === 'coherent' && observed.sampleRate === wanted.sampleRate &&
        observed.centerFrequency === frequency && observed.channelCount === count &&
        (gain === undefined || observed.gain === gain) && observed.reconfiguring === false;
      const handleStatus = frame => {
        const observed = normalizeStatus(frame);
        statusSequence += 1;
        lastStatus = observed;
        if (!initialStatus) {
          initialStatus = observed;
          receipt.before = publicStatus(observed);
          receipt.after = publicStatus(observed);
          try { validateInitialStatus(observed, wanted); }
          catch (error) { return fail(error); }
          operations = operationList(observed, wanted);
          if (readOnly && operations.length)
            return fail(receiverError('KRAKEN_STARTUP_MISMATCH',
              'Suite settings differ from saved receiver settings. Apply them before starting live processing.', 409));
          return next();
        }
        if (waitingForIdle) {
          if (!suiteIdle(observed)) return;
          waitingForIdle = false;
          return sendCurrent();
        }
        if (current && currentAcknowledged && statusSequence > acknowledgedAfterStatus &&
            current.matches(observed) && matchesTuple(observed, expectedReadback.frequency,
              expectedReadback.channelCount, expectedReadback.gain))
          acceptReadback();
      };
      const handleResponse = frame => {
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
        if (acknowledgedValue !== current.value && acknowledgedValue !== wireValue)
          return fail(receiverError('KRAKEN_ACKNOWLEDGEMENT_MISMATCH',
            `Suite V2 confirmed an unexpected ${current.field} value.`, 502,
            failureReceipt()));
        currentAcknowledged = true;
        // A status received before this exact ACK is never its readback.
        acknowledgedAfterStatus = statusSequence;
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

  return {synchronize, verify: config => synchronize(config, {readOnly: true})};
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
    }, writePath: 'Suite V2 TCP control for frequency and active element count only',
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
  requiresReceiverSynchronization};
