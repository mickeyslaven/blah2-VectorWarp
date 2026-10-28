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
  clear() { this.entries = []; this.bytes = 0; }
}

module.exports = {PresentationHistory, frameTimestamp};
