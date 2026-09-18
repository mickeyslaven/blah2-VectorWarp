'use strict';

// Histories consume completed data messages, not a second wall-clock poller.
// Each stream owns its timestamp: a delayed stream cannot duplicate an old frame
// simply because the processor's separate timestamp socket advanced first.
function history(limit, valid, render, compatible = () => true) {
  let frames = [];
  let output = null;
  return {
    update_data(message) {
      let frame;
      try { frame = typeof message === 'string' ? JSON.parse(message) : message; }
      catch (_) { return; }
      if (!frame || !Number.isFinite(frame.timestamp) || !valid(frame)) return;
      const last = frames[frames.length - 1];
      if (last && frame.timestamp <= last.timestamp) return;
      if (last && !compatible(last, frame)) frames = [];
      frames.push(frame);
      if (limit) frames = frames.slice(-limit);
      else frames = frames.filter(item => frame.timestamp - item.timestamp <= 300000);
      output = render(frames, frame);
    },
    get_data() { return output; }
  };
}

const matrixValid = frame => Array.isArray(frame.delay) && Array.isArray(frame.doppler) &&
  Array.isArray(frame.data) && frame.data.length === frame.doppler.length &&
  frame.data.length > 0 && frame.delay.length > 0 && frame.data.every(row =>
    Array.isArray(row) && row.length === frame.delay.length && row.every(Number.isFinite));

function maxhold() {
  return history(20, matrixValid, (frames, frame) => ({...frame,
    data: frame.data.map((row, i) => row.map((_, j) =>
      frames.reduce((max, item) => Math.max(max, item.data[i][j]), -Infinity)))
  }), (a, b) => JSON.stringify([a.delay, a.doppler]) === JSON.stringify([b.delay, b.doppler]));
}

function detection() {
  return history(null, frame => ['delay', 'doppler', 'snr'].every(key =>
    Array.isArray(frame[key]) && frame[key].length === frame.delay.length),
  (frames, frame) => ({
    frameTimestamp: frame.timestamp,
    timestamp: frames.flatMap(item => item.delay.map(() => item.timestamp)),
    delay: frames.flatMap(item => item.delay),
    doppler: frames.flatMap(item => item.doppler),
    snr: frames.flatMap(item => item.snr)
  }));
}

function iqdata() {
  return history(20, frame => Array.isArray(frame.frequency) && Array.isArray(frame.spectrum) &&
    frame.frequency.length === frame.spectrum.length,
  (frames, frame) => ({...frame,
    frameTimestamp: frame.timestamp,
    frequency: frames.map(item => item.frequency),
    spectrum: frames.map(item => item.spectrum),
    timestamp: frames.map(item => item.timestamp)
  }), (a, b) => JSON.stringify(a.frequency) === JSON.stringify(b.frequency));
}

function timing() {
  return history(20, () => true, (frames, frame) => {
    const keys = [...new Set(frames.flatMap(Object.keys))].filter(key =>
      !['uptime', 'nCpi', 'frameTimestamp', 'acceleration'].includes(key) &&
      frames.some(item => typeof item[key] === 'number' && Number.isFinite(item[key])));
    const result = {frameTimestamp: frame.timestamp};
    for (const key of keys) result[key] = frames.map(item => item[key] ?? null);
    return result;
  });
}

module.exports = {maxhold, detection, iqdata, timing};
