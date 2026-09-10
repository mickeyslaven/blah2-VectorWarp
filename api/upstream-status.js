'use strict';

const net = require('net');
const finite = value => typeof value === 'number' && Number.isFinite(value) ? value : null;

function compareKrakenStatus(config, live) {
  const expected = {
    centerFrequency: config.capture.fc,
    sampleRate: config.capture.fs,
    channels: config.capture.device.channel_count,
    mode: 'coherent'
  };
  const actual = {
    centerFrequency: finite(live.settings?.center_freq),
    sampleRate: finite(live.settings?.sample_rate),
    channels: finite(live.num_channels),
    maximumChannels: finite(live.max_elements),
    gain: finite(live.settings?.gain),
    mode: live.operating_mode,
    reconfiguring: live.reconfiguring === true,
    recovering: live.recovering === true,
    cooldownActive: live.cooldown_active === true,
    calibrationState: live.calibration_state || null
  };
  const issues = [];
  const mismatch = (field, label, wanted, found) => {
    if (found === null)
      issues.push({field, label, expected: wanted, actual: null,
        severity: 'warning', message: `${label} was not reported by the upstream service.`});
    else if (found !== wanted)
      issues.push({field, label, expected: wanted, actual: found,
        severity: 'error'});
  };
  mismatch('capture.fc', 'Center frequency', expected.centerFrequency,
    actual.centerFrequency);
  mismatch('capture.fs', 'Sample rate', expected.sampleRate,
    actual.sampleRate);
  mismatch('capture.device.channel_count', 'Channel count', expected.channels,
    actual.channels);
  if (actual.mode !== expected.mode)
    issues.push({field: 'capture.device.heimdall', label: 'Operating mode',
      expected: expected.mode, actual: actual.mode || 'not reported',
      severity: actual.mode ? 'error' : 'warning'});
  if (actual.maximumChannels && expected.channels > actual.maximumChannels)
    issues.push({field: 'capture.device.channel_count',
      label: 'Available receivers', expected: expected.channels,
      actual: actual.maximumChannels, severity: 'error'});
  if (actual.reconfiguring || actual.recovering || actual.cooldownActive)
    issues.push({field: 'capture.device.heimdall', label: 'Upstream state',
      expected: 'ready', actual: actual.reconfiguring ? 'reconfiguring' :
        actual.recovering ? 'recovering coherence' : 'calibrating',
      severity: 'warning'});
  if (actual.calibrationState &&
      !['CALIBRATED', 'CONVERGED'].includes(String(actual.calibrationState).toUpperCase()))
    issues.push({field: 'capture.device.heimdall', label: 'Calibration',
      expected: 'converged', actual: actual.calibrationState,
      severity: 'warning'});
  return {expected, actual, issues, matched: issues.length === 0};
}

function getUpstreamStatus(config, options = {}) {
  if (config.capture?.device?.type !== 'Kraken')
    return Promise.resolve({supported: false, receiverType:
      config.capture?.device?.type || 'Unknown',
    available: null, matched: false,
    message: 'No upstream telemetry is available for this receiver. Tuning, drivers and hardware are not verified by the settings API.'});
  if (!config.capture.device.heimdall || typeof config.capture.device.heimdall.host !== 'string')
    return Promise.resolve({supported: true, available: false, matched: false,
      message: 'Set the Heimdall host and status port in Receiver settings.'});
  const host = config.capture.device.heimdall.host;
  const dataPort = config.capture.device.heimdall.port;
  const controlPort = config.capture.device.heimdall.control_port ?? 8092;
  const timeoutMs = options.timeoutMs || 1600;
  const connector = options.connector || net.createConnection;
  return new Promise(resolve => {
    let settled = false;
    let buffer = '';
    let receivedBytes = 0;
    let socket;
    let deadline;
    const finish = result => {
      if (settled) return;
      settled = true;
      clearTimeout(deadline);
      if (socket) socket.destroy();
      resolve({supported: true, receiverType: 'Kraken', host, dataPort,
        controlPort, checkedAt: Date.now(), ...result});
    };
    try {
      socket = connector({host, port: controlPort});
    } catch (error) {
      finish({available: false, message: error.message});
      return;
    }
    socket.setEncoding('utf8');
    // Suite V2 broadcasts newline-delimited status. Observe only: never send
    // commands, retune the receiver, or connect to its IQ data stream.
    deadline = setTimeout(() => finish({available: false,
      message: 'No compatible status broadcast received. Check the status port and Suite version.'}), timeoutMs);
    socket.on('data', chunk => {
      receivedBytes += Buffer.byteLength(chunk);
      if (receivedBytes > 65536) return finish({available: false,
        message: 'Status response exceeded 64 KiB; check the selected status port.'});
      buffer += chunk;
      const lines = buffer.split('\n');
      buffer = lines.pop();
      for (const line of lines) {
        try {
          const live = JSON.parse(line);
          if (!live || !live.settings) continue;
          finish({available: true,
            ...compareKrakenStatus(config, live)});
          return;
        } catch (_) { /* Ignore unrelated or partial control responses. */ }
      }
    });
    socket.on('timeout', () => finish({available: false,
      message: `No status received from ${host}:${controlPort}.`}));
    socket.on('error', error => finish({available: false,
      message: ({ECONNREFUSED: 'Connection refused. Check that Suite V2 is running and the status port is correct.',
        ENOTFOUND: 'Host not found. Check the receiver address.',
        EAI_AGAIN: 'Host lookup failed. Check DNS or use the receiver IP address.',
        ETIMEDOUT: 'Connection timed out. Check the receiver address and network.',
        EHOSTUNREACH: 'Receiver host is unreachable. Check its address and network.',
        ENETUNREACH: 'Receiver network is unreachable.'})[error.code] || error.message}));
    socket.on('close', () => finish({available: false,
      message: `Connection to ${host}:${controlPort} closed before status arrived.`}));
  });
}

module.exports = {compareKrakenStatus, getUpstreamStatus};
