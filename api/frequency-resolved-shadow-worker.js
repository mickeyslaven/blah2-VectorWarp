'use strict';

const net = require('net');
const {parentPort, workerData} = require('worker_threads');
const {SpectralAccumulator, summarizeCovariances} = require('./frequency-resolved-analysis');

const MAGIC = 0x4d434851;

function capture(descriptor, options = {}) {
  return new Promise((resolve, reject) => {
    const accumulator = new SpectralAccumulator(descriptor);
    const requiredBlocks = Math.max(1, Math.ceil(descriptor.sample_rate_hz *
      (options.capture_seconds ?? .25) / descriptor.fft_size));
    let buffer = Buffer.alloc(0), packets = 0, invalidPackets = 0, finished = false;
    const socket = net.createConnection({host: options.host, port: options.port});
    const timeout = setTimeout(() => fail(new Error('Frequency-resolved capture timed out')),
      options.timeout_ms ?? 15000);
    const finish = () => {
      if (finished) return; finished = true; clearTimeout(timeout); socket.destroy();
      try { resolve(summarizeCovariances(accumulator.finish(), descriptor,
        {fft_blocks: accumulator.blocks, packets, invalid_packets: invalidPackets})); }
      catch (error) { reject(error); }
    };
    const fail = error => { if (!finished) { finished = true; clearTimeout(timeout); socket.destroy(); reject(error); } };
    socket.on('error', fail);
    socket.on('data', chunk => {
      buffer = Buffer.concat([buffer, chunk]);
      try {
        while (buffer.length >= 32 && accumulator.blocks < requiredBlocks) {
          const magic = buffer.readUInt32BE(0), channels = buffer.readUInt32BE(4);
          const samples = buffer.readUInt32BE(8), phase = buffer.readUInt32BE(12);
          const noise = buffer.readUInt32BE(16), retune = buffer.readUInt32BE(28);
          if (magic !== MAGIC || channels < 1 || channels > 16 || samples < 1 || samples > 10000000)
            throw new Error('Malformed MCHQ header');
          const bytes = 32 + channels * 8 + channels * samples * 2;
          if (buffer.length < bytes) break;
          const frame = buffer.subarray(0, bytes); buffer = buffer.subarray(bytes); ++packets;
          const frequencies = Array.from({length: channels}, (_, channel) =>
            frame.readFloatLE(32 + channel * 8));
          const valid = channels === descriptor.capture_channels && (phase & 255) === 4 &&
            !(phase & 0xe00) && noise === 0 && retune === 0 && frequencies.every(frequency =>
              Math.abs(frequency - descriptor.frequency_hz) <= Math.max(1, descriptor.frequency_hz * 1e-6));
          if (!valid) { ++invalidPackets; continue; }
          const payload = 32 + channels * 8, size = descriptor.fft_size;
          const means = Array.from({length: channels}, (_, channel) => {
            let real = 0, imaginary = 0;
            for (let sample = 0; sample < samples; ++sample) {
              const at = payload + (channel * samples + sample) * 2;
              real += frame[at]; imaginary += frame[at + 1];
            }
            return [real / samples, imaginary / samples];
          });
          for (let first = 0; first + size <= samples && accumulator.blocks < requiredBlocks; first += size) {
            const block = descriptor.channel_indices.map(channel => {
              const real = new Float64Array(size), imaginary = new Float64Array(size);
              for (let sample = 0; sample < size; ++sample) {
                const at = payload + (channel * samples + first + sample) * 2;
                real[sample] = (frame[at] - means[channel][0]) / 127.5;
                imaginary[sample] = (frame[at + 1] - means[channel][1]) / 127.5;
              }
              return {real, imaginary};
            });
            accumulator.addBlock(block);
          }
        }
        if (accumulator.blocks >= requiredBlocks) finish();
      } catch (error) { fail(error); }
    });
  });
}

if (parentPort) capture(workerData.descriptor, workerData.options)
  .then(result => parentPort.postMessage({ok: true, result}),
    error => parentPort.postMessage({ok: false, error: error.message}));

module.exports = {capture};
