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
  let frames = []; let first = 0; let lastTimestamp = -Infinity; let frameTimestamp = null;
  let dirty = true; let output = null;
  const valid = frame => ['delay', 'doppler', 'snr'].every(key => Array.isArray(frame[key]) && frame[key].length === frame.delay.length);
  const expire = timestamp => {
    while (first < frames.length && timestamp - frames[first].timestamp > 300000) {
      frames[first++] = null; // Release payloads immediately; compaction only removes empty slots.
    }
    if (first === frames.length) { frames = []; first = 0; }
    // Avoid retaining an ever-growing consumed prefix during high-rate sparse detection streams.
    if (first > 1024 && first * 2 >= frames.length) { frames = frames.slice(first); first = 0; }
  };
  return {
    update_data(message) {
      let frame; try { frame = typeof message === 'string' ? JSON.parse(message) : message; } catch (_) { return; }
      if (!frame || !Number.isFinite(frame.timestamp) || !valid(frame) || frame.timestamp <= lastTimestamp) return;
      lastTimestamp = frame.timestamp; frameTimestamp = frame.timestamp; expire(frame.timestamp);
      // Empty detector frames advance expiry and the completed-message timestamp,
      // but carry no data and must not consume five minutes of history each.
      if (frame.delay.length) frames.push({timestamp: frame.timestamp, delay: [...frame.delay], doppler: [...frame.doppler], snr: [...frame.snr]});
      output = null; dirty = true;
    },
    get_data() {
      if (!dirty) return output;
      const timestamp = []; const delay = []; const doppler = []; const snr = [];
      for (let i = first; i < frames.length; i++) {
        const frame = frames[i];
        for (let j = 0; j < frame.delay.length; j++) { timestamp.push(frame.timestamp); delay.push(frame.delay[j]); doppler.push(frame.doppler[j]); snr.push(frame.snr[j]); }
      }
      output = frameTimestamp === null ? null : Object.freeze({frameTimestamp, timestamp: Object.freeze(timestamp), delay: Object.freeze(delay), doppler: Object.freeze(doppler), snr: Object.freeze(snr)}); dirty = false;
      return output;
    }
  };
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
      !['uptime', 'nCpi', 'frameTimestamp', 'acceleration'].includes(key));
    const result = {frameTimestamp: frame.timestamp};
    for (const key of keys) result[key] = frames.map(item => item[key] ?? null);
    return result;
  });
}

module.exports = {maxhold, detection, iqdata, timing};
