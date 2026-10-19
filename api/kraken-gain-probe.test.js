'use strict';

const assert = require('node:assert/strict');
const {test} = require('node:test');
const {EventEmitter} = require('node:events');
const net = require('node:net');
const {measureGain, captureGain, packetMetrics, LIMITS} = require('./kraken-gain-probe');

function config(overrides = {}) {
  return {capture: {fc: 527000001, fs: 40, replay: {state: false},
    device: {type: 'Kraken', channel_count: 2,
      heimdall: {host: '127.0.0.1', port: 1, gain: 14.4}}, ...overrides}};
}

function frame(c, {samples = 6, channels = c.capture.device.channel_count, phase = 4,
  noise = 0, retune = 0, counter = 7, group = 0, frequency, gain,
  sample = (channel, index) => [128 + channel + index, 127 - channel - index]} = {}) {
  const buffer = Buffer.alloc(32 + channels * 8 + channels * samples * 2);
  [0x4d434851, channels, samples, phase, noise, counter, group, retune]
    .forEach((value, index) => buffer.writeUInt32BE(value, index * 4));
  for (let channel = 0; channel < channels; ++channel) {
    buffer.writeFloatLE(frequency?.[channel] ?? c.capture.fc, 32 + channel * 8);
    buffer.writeFloatLE(gain?.[channel] ?? c.capture.device.heimdall.gain, 36 + channel * 8);
    for (let index = 0; index < samples; ++index) {
      const at = 32 + channels * 8 + (channel * samples + index) * 2;
      const [i, q] = sample(channel, index); buffer[at] = i; buffer[at + 1] = q;
    }
  }
  return buffer;
}

function injected(c = config(), options = {}) {
  const socket = new EventEmitter();
  socket.destroyed = false;
  socket.destroy = () => { socket.destroyed = true; };
  socket.write = () => { throw new Error('Probe must never write control data'); };
  const promise = captureGain(c, options, {connect(endpoint) {
    assert.deepEqual(endpoint, {host: c.capture.device.heimdall.host, port: c.capture.device.heimdall.port});
    return socket;
  }});
  return {socket, promise};
}

async function loopback(t, onConnection) {
  const sockets = new Set();
  const server = net.createServer(socket => {
    sockets.add(socket); socket.once('close', () => sockets.delete(socket));
    socket.on('error', () => {});
    socket.on('data', () => assert.fail('Probe sent unexpected bytes'));
    onConnection(socket);
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject); server.listen(0, '127.0.0.1', resolve);
  });
  t.after(async () => {
    for (const socket of sockets) socket.destroy();
    await new Promise(resolve => server.close(resolve));
  });
  return server.address().port;
}

test('wire metrics preserve signed component DC, component clipping and rare peaks', () => {
  const samples = 2000, payload = Buffer.alloc(3 * samples * 2);
  for (let n = 0; n < samples; ++n) {
    payload[n * 2] = n % 2 ? 255 : 0; payload[n * 2 + 1] = n % 2 ? 128 : 127;
    payload[(samples + n) * 2] = 191; payload[(samples + n) * 2 + 1] = 64;
    payload[(samples * 2 + n) * 2] = n < 3 ? 255 : 128;
    payload[(samples * 2 + n) * 2 + 1] = 128;
  }
  const channels = packetMetrics(payload, 3, samples);
  assert.equal(channels[0].railFraction, .5);
  assert.equal(channels[0].components.i.railFraction, 1);
  assert.equal(channels[0].components.q.railFraction, 0);
  assert.equal(channels[0].rmsFraction, 1);
  assert.equal(channels[0].meanI, 0);
  assert.equal(channels[0].meanQ, 0);
  assert.equal(channels[1].meanI, 63.5 / 127.5);
  assert.equal(channels[1].meanQ, -63.5 / 127.5);
  assert.equal(channels[1].rmsFraction, 63.5 / 127.5);
  assert.equal(channels[2].railFraction, 3 / 4000);
  assert.equal(channels[2].p999Fraction, 1, 'I-only rare peaks must not be diluted by quiet Q');
  assert.equal(channels[2].components.q.p999Fraction, .5 / 127.5);
  assert.throws(() => packetMetrics(payload, 17, samples), /dimensions/);
});

test('worker probe parses fragmented loopback frames, all channels and exact sample limit', async t => {
  const c = config();
  const bytes = Buffer.concat([frame(c), frame(c, {sample: (ch, n) => n < 4 ? [255, 0] : [128, 128]})]);
  c.capture.device.heimdall.port = await loopback(t, socket => {
    let offset = 0, piece = 0;
    function send() {
      if (socket.destroyed || offset === bytes.length) return;
      const count = [1, 3, 17, 11, 2, 31][piece++ % 6];
      socket.write(bytes.subarray(offset, offset + count)); offset = Math.min(offset + count, bytes.length);
      setImmediate(send);
    }
    send();
  });
  const result = await measureGain(c, {timeoutMs: 3000});
  assert.equal(result.packets, 2);
  assert.equal(result.samplesPerChannel, 10);
  assert.equal(result.captureSeconds, .25);
  assert.equal(result.frequencyHz, c.capture.fc);
  assert.equal(result.gainDb, 14.4, 'float32 metadata rounding is accepted');
  assert.equal(result.frequencyChangeCounter, 7);
  assert.equal(result.channels.length, 2);
  for (const channel of result.channels) assert.equal(channel.railFraction, .4);
  assert.equal(result.worstCase.railFraction, .4);
  assert.equal(result.worstCase.peakFraction, 1);
  assert.equal(result.source, 'calibrated_mchq_uint8_wire');
  assert.equal(result.sampleRateVerifiedByStream, false);
  assert.equal(result.sampleRateSource, 'config');
  assert.ok(result.elapsedMs < 3000);
});

test('rejects missing expectations, non-fixed gain and unbounded options before connecting', async () => {
  const mutations = [c => { delete c.capture.fc; }, c => { delete c.capture.fs; },
    c => { c.capture.device.channel_count = 17; }, c => { c.capture.device.type = 'RspDuo'; },
    c => { c.capture.replay.state = true; }, c => { c.capture.device.heimdall.gain = 'keep'; },
    c => { c.capture.device.heimdall.gain = -1; }, c => { delete c.capture.device.heimdall.gain; },
    c => { c.capture.device.heimdall.gain = NaN; }, c => { c.capture.device.heimdall.host = 'http://example.com'; }];
  for (const mutate of mutations) {
    const c = config(); mutate(c);
    await assert.rejects(captureGain(c, {}, {connect: () => assert.fail('Invalid config connected')}),
      {code: 'KRAKEN_GAIN_PROBE_CONFIG'});
  }
  for (const options of [{captureSeconds: .249}, {captureSeconds: .501}, {timeoutMs: 15001}, {timeoutMs: 0}])
    await assert.rejects(measureGain(config(), options), {code: 'KRAKEN_GAIN_PROBE_CONFIG'});
});

test('rejects mismatched/nonfinite metadata on every channel without using payload', async () => {
  const c = config();
  for (const change of [{channels: 1}, {frequency: [c.capture.fc, c.capture.fc + 1000]},
    {gain: [14.4, 14.5]}, {frequency: [NaN, c.capture.fc]}, {gain: [Infinity, 14.4]}]) {
    const {socket, promise} = injected(c);
    const rejection = assert.rejects(promise, {code: 'KRAKEN_GAIN_PROBE_MISMATCH'});
    socket.emit('data', frame(c, change).subarray(0, 32 + (change.channels ?? 2) * 8));
    await rejection; assert.equal(socket.destroyed, true);
  }
});

test('rejects uncalibrated, stale/wideband/independent, noise and retuning frames', async () => {
  const c = config();
  for (const change of [{phase: 3}, {phase: 4 | 0x200}, {phase: 4 | 0x400},
    {phase: 4 | 0x800}, {noise: 1}, {retune: 1}]) {
    const {socket, promise} = injected(c);
    const rejection = assert.rejects(promise, {code: 'KRAKEN_GAIN_PROBE_UNCALIBRATED'});
    socket.emit('data', frame(c, change).subarray(0, 32));
    await rejection;
  }
  const {socket, promise} = injected(c);
  socket.emit('data', frame(c, {samples: 10, phase: 4 | 0x100}));
  assert.equal((await promise).samplesPerChannel, 10, 'manual calibration flag is compatible with synchronized data');
});

test('rejects frequency counter or group changes after the first accepted packet', async () => {
  const c = config();
  for (const change of [{counter: 8}, {group: 1}]) {
    const {socket, promise} = injected(c);
    const rejection = assert.rejects(promise, {code: 'KRAKEN_GAIN_PROBE_MISMATCH'});
    socket.emit('data', Buffer.concat([frame(c), frame(c, change)]));
    await rejection;
  }
});

test('bounded assembly handles coalesced maximal packets and rejects forged dimensions immediately', async () => {
  const c = config({fs: 2400000});
  c.capture.device.channel_count = 1;
  const one = frame(c, {samples: LIMITS.maxPacketSamples, sample: () => [127, 128]});
  const {socket, promise} = injected(c);
  socket.emit('data', Buffer.concat([one, one, one]));
  const result = await promise;
  assert.equal(result.samplesPerChannel, 600000);
  assert.equal(result.packets, 3);
  assert.equal(result.peakBufferedBytes, one.length);
  assert.equal(result.maxBufferedBytes, one.length);
  assert.ok(result.maxBufferedBytes <= LIMITS.maxFrameBytes);
  for (const [offset, value] of [[4, 17], [4, 0], [8, LIMITS.maxPacketSamples + 1], [8, 0], [0, 1]]) {
    const header = Buffer.from(one.subarray(0, 32)); header.writeUInt32BE(value, offset);
    const next = injected(c);
    const rejection = assert.rejects(next.promise, {code: 'KRAKEN_GAIN_PROBE_PROTOCOL'});
    next.socket.emit('data', header);
    await rejection; assert.equal(next.socket.destroyed, true);
  }
});

test('timeout, early EOF and cancellation fail closed and destroy the socket', async () => {
  const timeout = injected(config(), {timeoutMs: 20});
  await assert.rejects(timeout.promise, {code: 'KRAKEN_GAIN_PROBE_TIMEOUT'});
  assert.equal(timeout.socket.destroyed, true);
  const short = injected();
  const rejection = assert.rejects(short.promise, {code: 'KRAKEN_GAIN_PROBE_CONNECTION'});
  short.socket.emit('data', Buffer.alloc(5)); short.socket.emit('end');
  await rejection; assert.equal(short.socket.destroyed, true);
  const controller = new AbortController(), aborted = injected(config(), {signal: controller.signal});
  const abortRejection = assert.rejects(aborted.promise, {code: 'KRAKEN_GAIN_PROBE_ABORTED'});
  controller.abort(); await abortRejection; assert.equal(aborted.socket.destroyed, true);
});

test('worker total deadline and cancellation terminate stalled capture', async t => {
  const c = config();
  c.capture.device.heimdall.port = await loopback(t, () => {});
  await assert.rejects(measureGain(c, {timeoutMs: 200}), {code: 'KRAKEN_GAIN_PROBE_TIMEOUT'});
  const controller = new AbortController();
  const pending = measureGain(c, {timeoutMs: 3000, signal: controller.signal});
  const rejection = assert.rejects(pending, {code: 'KRAKEN_GAIN_PROBE_ABORTED'});
  controller.abort(); await rejection;
});
