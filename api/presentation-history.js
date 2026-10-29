'use strict';

function frameTimestamp(frame) {
  let value = frame;
  if (typeof frame === 'string') {
    try { value = JSON.parse(frame); } catch (_) { return null; }
  }
  const raw = value?.frameTimestamp ?? value?.timestamp;
  const timestamp = Array.isArray(raw) ? raw.at(-1) : raw;
  const number = timestamp instanceof Date ? timestamp.getTime() : Number(timestamp);
  return Number.isFinite(number) ? number : null;
}

class PresentationHistory {
  constructor({maxAgeMs = 15000, maxBytes = 48 * 1024 * 1024} = {}) {
    this.maxAgeMs = maxAgeMs;
    this.maxBytes = maxBytes;
    this.entries = [];
    this.bytes = 0;
  }
  add(frame, arrivedAt = Date.now()) {
    const timestamp = frameTimestamp(frame);
    if (timestamp === null) return;
    const bytes = typeof frame === 'string' ? Buffer.byteLength(frame) :
      Buffer.byteLength(JSON.stringify(frame));
    this.entries.push({timestamp, arrivedAt, frame, bytes});
    this.bytes += bytes;
    const oldestArrival = arrivedAt - this.maxAgeMs;
    while (this.entries.length > 1 &&
      (this.entries[0].arrivedAt < oldestArrival || this.bytes > this.maxBytes)) {
      this.bytes -= this.entries.shift().bytes;
    }
  }
  at(timestamp) {
    let match = null;
    for (const entry of this.entries) {
      if (entry.timestamp <= timestamp && (!match || entry.timestamp > match.timestamp)) match = entry;
    }
    return match?.frame ?? null;
  }
  timestampAt(timestamp) {
    let match = null;
    for (const entry of this.entries) {
      if (entry.timestamp <= timestamp && (!match || entry.timestamp > match.timestamp)) match = entry;
    }
    return match?.timestamp ?? null;
  }
  exact(timestamp) {
    for (let index = this.entries.length - 1; index >= 0; index -= 1) {
      if (this.entries[index].timestamp === timestamp) return this.entries[index].frame;
    }
    return null;
  }
  diagnostics(expectedIntervalMs = null) {
    const timestamps = this.entries.map(entry => entry.timestamp);
    const intervals = timestamps.slice(1).map((value, index) => value - timestamps[index])
      .filter(value => Number.isFinite(value) && value > 0);
    const sorted = [...intervals].sort((left, right) => left - right);
    const percentile = fraction => sorted.length ?
      sorted[Math.min(sorted.length - 1, Math.floor((sorted.length - 1) * fraction))] : null;
    const toleranceMs = Number.isFinite(expectedIntervalMs) ? 100 : null;
    return {
      samples: timestamps.length,
      latest_timestamp: timestamps.at(-1) ?? null,
      interval_median_ms: percentile(.5),
      interval_p95_ms: percentile(.95),
      interval_max_ms: sorted.at(-1) ?? null,
      expected_interval_ms: Number.isFinite(expectedIntervalMs) ? expectedIntervalMs : null,
      within_100ms_fraction: toleranceMs === null || !intervals.length ? null :
        intervals.filter(value => Math.abs(value - expectedIntervalMs) <= toleranceMs).length /
          intervals.length
    };
  }
  clear() { this.entries = []; this.bytes = 0; }
}

module.exports = {PresentationHistory, frameTimestamp};
