'use strict';

const net = require('net');
const {performance} = require('perf_hooks');
const {Worker, isMainThread, parentPort, workerData} = require('worker_threads');

const LIMITS = Object.freeze({maxChannels: 16, maxPacketSamples: 262144,
  minCaptureSeconds: .25, maxCaptureSeconds: .5, maxTimeoutMs: 15000,
  maxFrameBytes: 32 + 16 * 8 + 16 * 262144 * 2});
const MAGIC = 0x4d434851;
const CENTER = 127.5;

function probeError(code, message) {
  const error = new Error(message);
  error.code = `KRAKEN_GAIN_PROBE_${code}`;
  return error;
}

function descriptorFrom(config) {
  const capture = config?.capture, device = capture?.device, endpoint = device?.heimdall;
  const integer = (value, max) => Number.isSafeInteger(value) && value > 0 && value <= max;
  if (device?.type !== 'Kraken' || capture?.replay?.state === true)
    throw probeError('CONFIG', 'Gain probing requires a live Kraken profile.');
  if (!integer(capture.fc, 0xffffffff) || !integer(capture.fs, 0xffffffff) ||
      !integer(device.channel_count, LIMITS.maxChannels))
    throw probeError('CONFIG', 'Gain probing requires known frequency, sample rate, and 1–16 channels.');
  if (!endpoint || typeof endpoint.host !== 'string' ||
      (!net.isIP(endpoint.host) && !/^(?=.{1,253}$)[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?$/i.test(endpoint.host)) ||
      !integer(endpoint.port, 65535))
    throw probeError('CONFIG', 'Gain probing requires a valid Heimdall IQ host and port.');
  if (typeof endpoint.gain !== 'number' || !Number.isFinite(endpoint.gain) ||
      endpoint.gain < 0 || endpoint.gain > 50)
    throw probeError('CONFIG', 'Gain probing requires a known common fixed gain from 0 through 50 dB.');
  return {frequencyHz: capture.fc, sampleRateHz: capture.fs, channelCount: device.channel_count,
    gainDb: endpoint.gain, host: endpoint.host, port: endpoint.port};
}

function captureOptions(options = {}) {
  const captureSeconds = options.captureSeconds ?? .25;
  const timeoutMs = options.timeoutMs ?? LIMITS.maxTimeoutMs;
  if (!Number.isFinite(captureSeconds) || captureSeconds < LIMITS.minCaptureSeconds ||
      captureSeconds > LIMITS.maxCaptureSeconds || !Number.isFinite(timeoutMs) ||
      timeoutMs < 1 || timeoutMs > LIMITS.maxTimeoutMs)
    throw probeError('CONFIG', 'Gain probe capture must be 0.25–0.5 s with a 1–15000 ms deadline.');
  return {captureSeconds, timeoutMs};
}

// Fractions use the fixed wire midpoint and full scale, without subtracting DC.
// These are delivered calibrated uint8 samples, not raw ADC measurements.
function componentMetrics(histogram, samples) {
  let sum = 0, sumSquares = 0, peak = 0;
  for (let value = 0; value < 256; ++value) {
    const count = histogram[value], centered = value - CENTER;
    sum += count * centered;
    sumSquares += count * centered * centered;
    if (count) peak = Math.max(peak, Math.abs(centered));
  }
  let cumulative = 0, percentile = 0;
  const rank = Math.ceil(samples * .999);
  for (let distance = 0; distance < 128; ++distance) {
    cumulative += histogram[127 - distance] + histogram[128 + distance];
    if (cumulative >= rank) { percentile = distance + .5; break; }
  }
  return {railFraction: (histogram[0] + histogram[255]) / samples,
    peakFraction: peak / CENTER, p999Fraction: percentile / CENTER,
    rmsFraction: Math.sqrt(sumSquares / samples) / CENTER, mean: sum / samples / CENTER};
}

function summarize(histograms, samples) {
  return histograms.map(({i, q}, channel) => {
    const I = componentMetrics(i, samples), Q = componentMetrics(q, samples);
    return {channel, samples, railFraction: (I.railFraction + Q.railFraction) / 2,
      peakFraction: Math.max(I.peakFraction, Q.peakFraction),
      p999Fraction: Math.max(I.p999Fraction, Q.p999Fraction),
      rmsFraction: Math.max(I.rmsFraction, Q.rmsFraction), meanI: I.mean, meanQ: Q.mean,
      components: {i: I, q: Q}};
  });
}

function newHistograms(channels) {
  return Array.from({length: channels}, () => ({i: new Float64Array(256), q: new Float64Array(256)}));
}

function addPayload(histograms, payload, packetSamples, take) {
  for (let channel = 0; channel < histograms.length; ++channel) {
    const {i, q} = histograms[channel];
    const start = channel * packetSamples * 2;
    for (let sample = 0; sample < take; ++sample) {
      ++i[payload[start + sample * 2]];
      ++q[payload[start + sample * 2 + 1]];
    }
  }
}

// Pure helper for channel-major, interleaved I/Q payloads (no MCHQ header).
function packetMetrics(payload, channelCount, samples) {
  if (!Buffer.isBuffer(payload) || !Number.isInteger(channelCount) || channelCount < 1 ||
      channelCount > LIMITS.maxChannels || !Number.isInteger(samples) || samples < 1 ||
      samples > LIMITS.maxPacketSamples || payload.length !== channelCount * samples * 2)
    throw probeError('PROTOCOL', 'Invalid MCHQ payload dimensions.');
  const histograms = newHistograms(channelCount);
  addPayload(histograms, payload, samples, samples);
  return summarize(histograms, samples);
}

function decodeHeader(buffer, descriptor, firstHeader) {
  const channels = buffer.readUInt32BE(4), samples = buffer.readUInt32BE(8);
  if (buffer.readUInt32BE(0) !== MAGIC || channels < 1 || channels > LIMITS.maxChannels ||
      samples < 1 || samples > LIMITS.maxPacketSamples)
    throw probeError('PROTOCOL', 'Malformed or oversized MCHQ frame.');
  if (channels !== descriptor.channelCount)
    throw probeError('MISMATCH', 'MCHQ channel count differs from the requested settings.');
  const phase = buffer.readUInt32BE(12);
  if ((phase & 255) !== 4 || (phase & 0xe00) || buffer.readUInt32BE(16) !== 0 || buffer.readUInt32BE(28) !== 0)
    throw probeError('UNCALIBRATED', 'MCHQ frame is uncalibrated, stale, in another mode, noisy, or retuning.');
  const frequencyChangeCounter = buffer.readUInt32BE(20), groupIndex = buffer.readUInt32BE(24);
  if (firstHeader && (frequencyChangeCounter !== firstHeader.frequencyChangeCounter ||
      groupIndex !== firstHeader.groupIndex))
    throw probeError('MISMATCH', 'MCHQ frequency counter or channel group changed during the probe.');
  return {channels, samples, frequencyChangeCounter, groupIndex};
}

function validateMetadata(buffer, descriptor) {
  for (let channel = 0; channel < descriptor.channelCount; ++channel) {
    const frequency = buffer.readFloatLE(32 + channel * 8);
    const gain = buffer.readFloatLE(36 + channel * 8);
    // The protocol serializes frequency and gain as little-endian float32.
    // Compare frequency to its representable wire value, not unrounded Hz.
    const gainTolerance = Math.max(1e-6, Math.abs(descriptor.gainDb) * 2 ** -23);
    if (!Number.isFinite(frequency) || frequency !== Math.fround(descriptor.frequencyHz) ||
        !Number.isFinite(gain) || Math.abs(gain - descriptor.gainDb) > gainTolerance)
      throw probeError('MISMATCH', `MCHQ frequency or fixed gain differs on channel ${channel}.`);
  }
}

function capture(descriptor, options, connect, signal) {
  return new Promise((resolve, reject) => {
    const started = performance.now(), startedAt = new Date().toISOString();
    const targetSamples = Math.ceil(descriptor.sampleRateHz * options.captureSeconds);
    const histograms = newHistograms(descriptor.channelCount);
    // A single bounded assembly buffer; never concatenate queued TCP chunks.
    const maxBufferedBytes = 32 + descriptor.channelCount * 8 +
      descriptor.channelCount * LIMITS.maxPacketSamples * 2;
    const buffer = Buffer.allocUnsafe(maxBufferedBytes);
    let socket, timer, settled = false, filled = 0, needed = 32, stage = 'header';
    let header, firstHeader, samples = 0, packets = 0, peakBufferedBytes = 0;
    const abort = () => fail(probeError('ABORTED', 'Gain probe aborted.'));
    function cleanup() {
      clearTimeout(timer);
      signal?.removeEventListener('abort', abort);
      socket?.destroy();
    }
    function fail(error) {
      if (settled) return;
      settled = true; cleanup(); reject(error);
    }
    function finish() {
      const channels = summarize(histograms, samples);
      const worstCase = Object.fromEntries(['railFraction', 'peakFraction', 'p999Fraction', 'rmsFraction']
        .map(key => [key, Math.max(...channels.map(channel => channel[key]))]));
      settled = true; cleanup();
      resolve({frequencyHz: descriptor.frequencyHz, sampleRateHz: descriptor.sampleRateHz,
        channelCount: descriptor.channelCount, gainDb: descriptor.gainDb, channels, worstCase,
        packets, samplesPerChannel: samples, captureSeconds: samples / descriptor.sampleRateHz,
        startedAt, captureElapsedMs: performance.now() - started,
        frequencyChangeCounter: firstHeader.frequencyChangeCounter, groupIndex: firstHeader.groupIndex,
        source: 'calibrated_mchq_uint8_wire', sampleRateSource: 'config',
        calibration: {phaseState: 4, noiseSource: false, retuning: false, verifiedPackets: packets},
        sampleRateVerifiedByStream: false, wireCenter: CENTER, wireFullScale: CENTER,
        meanUnits: 'signed_fraction_of_wire_full_scale',
        componentAggregation: 'railFraction is across I/Q; peak, p999 and RMS are the worse component',
        continuity: 'TCP order only; MCHQ has no hardware sample counter',
        maxBufferedBytes, peakBufferedBytes});
    }
    function onData(chunk) {
      if (settled) return;
      try {
        if (!Buffer.isBuffer(chunk)) throw probeError('PROTOCOL', 'MCHQ data must be binary.');
        let offset = 0;
        while (offset < chunk.length && !settled) {
          if (performance.now() - started >= options.timeoutMs)
            throw probeError('TIMEOUT', 'Gain probe exceeded its deadline.');
          const count = Math.min(needed - filled, chunk.length - offset);
          chunk.copy(buffer, filled, offset, offset + count);
          filled += count; offset += count;
          peakBufferedBytes = Math.max(peakBufferedBytes, filled);
          if (filled < needed) continue;
          if (stage === 'header') {
            header = decodeHeader(buffer, descriptor, firstHeader);
            needed = 32 + header.channels * 8; stage = 'metadata';
          } else if (stage === 'metadata') {
            validateMetadata(buffer, descriptor);
            needed += header.channels * header.samples * 2; stage = 'payload';
          } else {
            const take = Math.min(header.samples, targetSamples - samples);
            addPayload(histograms, buffer.subarray(32 + header.channels * 8, needed), header.samples, take);
            firstHeader ??= header;
            samples += take; ++packets;
            if (samples >= targetSamples) finish();
            else { filled = 0; needed = 32; stage = 'header'; }
          }
        }
      } catch (error) { fail(error); }
    }
    if (signal?.aborted) { abort(); return; }
    signal?.addEventListener('abort', abort, {once: true});
    timer = setTimeout(() => fail(probeError('TIMEOUT', 'Gain probe timed out.')), options.timeoutMs);
    try {
      socket = connect({host: descriptor.host, port: descriptor.port});
      socket.on('data', onData);
      socket.once('error', error => fail(probeError('CONNECTION', error.message)));
      socket.once('end', () => fail(probeError('CONNECTION', 'MCHQ stream ended before the probe completed.')));
      socket.once('close', () => fail(probeError('CONNECTION', 'MCHQ stream closed before the probe completed.')));
    } catch (error) { fail(error); }
  });
}

// Socket injection is for deterministic tests. Production callers use measureGain.
async function captureGain(config, options = {}, {connect = net.createConnection} = {}) {
  return capture(descriptorFrom(config), captureOptions(options), connect, options.signal);
}

/**
 * Measure every configured channel on a fresh IQ connection in a worker thread.
 * The caller must separately confirm upstream sample rate: MCHQ has no rate field.
 * Resolves only a complete, calibrated, metadata-matched 0.25–0.5 s capture.
 * Does not write control commands, change settings, or claim raw ADC headroom.
 */
async function measureGain(config, options = {}) {
  const descriptor = descriptorFrom(config), normalized = captureOptions(options);
  return new Promise((resolve, reject) => {
    const started = performance.now();
    let worker, timer, settled = false;
    const abort = () => finish(probeError('ABORTED', 'Gain probe aborted.'));
    function finish(error, result) {
      if (settled) return;
      settled = true; clearTimeout(timer); options.signal?.removeEventListener('abort', abort);
      const terminated = worker ? worker.terminate() : Promise.resolve();
      terminated.then(() => error ? reject(error) : resolve({...result, elapsedMs: performance.now() - started}),
        terminationError => reject(error || terminationError));
    }
    if (options.signal?.aborted) { abort(); return; }
    options.signal?.addEventListener('abort', abort, {once: true});
    timer = setTimeout(() => finish(probeError('TIMEOUT', 'Gain probe exceeded its total deadline.')), normalized.timeoutMs);
    try {
      worker = new Worker(__filename, {workerData: {kind: 'kraken-gain-probe', descriptor, options: normalized}});
      worker.once('message', message => {
        if (message.ok) finish(null, message.result);
        else {
          const error = new Error(message.error); error.code = message.code; finish(error);
        }
      });
      worker.once('error', error => finish(error));
      worker.once('exit', code => {
        if (!settled) finish(probeError('WORKER', `Gain probe worker exited before returning a result (${code}).`));
      });
    } catch (error) { finish(error); }
  });
}

if (!isMainThread && workerData?.kind === 'kraken-gain-probe') {
  capture(workerData.descriptor, workerData.options, net.createConnection)
    .then(result => parentPort.postMessage({ok: true, result}),
      error => parentPort.postMessage({ok: false, error: error.message, code: error.code}));
}

module.exports = {measureGain, captureGain, packetMetrics, LIMITS};
