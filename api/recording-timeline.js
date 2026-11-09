'use strict';
const fs = require('fs');
const zlib = require('zlib');

const mapSummary = map => map && typeof map === 'object' ? {
  timestamp: map.timestamp ?? null, nRows: map.nRows ?? null, nCols: map.nCols ?? null,
  noisePower: map.noisePower ?? null, maxPower: map.maxPower ?? null,
  delay: map.delay ?? null, doppler: map.doppler ?? null,
  eventTime: map.eventTime ?? null
} : null;

async function eventTruthAt(source, kind, timestamp) {
  if (!source || !Number.isFinite(timestamp)) throw new Error('Recording truth time is invalid');
  if (typeof source.getAt === 'function') return source.getAt(kind, timestamp);
  if (typeof source.get === 'function') return source.get(kind);
  throw new Error('Recording truth source is unavailable');
}

class RecordingTimeline {
  constructor({maxAgeMs = 30000, io = fs} = {}) {
    this.maxAgeMs = maxAgeMs; this.io = io; this.frames = [];
    this.pending = new Set(); this.reservations = [];
    this.cache = {file: '', mtimeMs: 0, value: null};
    this.manual = null;
  }
  track(timestamp, promise) {
    const item = {timestamp, promise: null};
    item.promise = Promise.resolve(promise).finally(() => this.pending.delete(item));
    this.pending.add(item);
    return item.promise;
  }
  async settle(startMs, endMs) {
    for (;;) {
      const relevant = [...this.pending].filter(item =>
        item.timestamp >= startMs && item.timestamp <= endMs);
      if (!relevant.length) return;
      await Promise.allSettled(relevant.map(item => item.promise));
    }
  }
  reserve(startMs, endMs) {
    if (!Number.isFinite(startMs) || !Number.isFinite(endMs) || endMs < startMs) return;
    const overlapping = this.reservations.filter(item =>
      startMs <= item.endMs && endMs >= item.startMs);
    const frames = [...this.frames.filter(frame =>
      frame.timestamp_ms >= startMs && frame.timestamp_ms <= endMs),
      ...overlapping.flatMap(item => item.frames)];
    this.reservations = this.reservations.filter(item => !overlapping.includes(item));
    const unique = new Map();
    for (const frame of frames) {
      const identity = typeof frame.illuminator === 'string' ? frame.illuminator : '';
      unique.set(`${frame.timestamp_ms}:${identity}`, frame);
    }
    this.reservations.push({startMs: Math.min(startMs,
      ...overlapping.map(item => item.startMs)), endMs: Math.max(endMs,
      ...overlapping.map(item => item.endMs)), frames: [...unique.values()]});
    while (this.reservations.length > 64) this.reservations.shift();
  }
  add(frame, now = Date.now()) {
    if (!Number.isSafeInteger(frame?.timestamp_ms) || frame.timestamp_ms < 0) return;
    this.frames.push(frame);
    while (this.frames.length > 1 && this.frames[0].timestamp_ms < now - this.maxAgeMs)
      this.frames.shift();
    for (const reservation of this.reservations)
      if (frame.timestamp_ms >= reservation.startMs && frame.timestamp_ms <= reservation.endMs)
        reservation.frames.push(frame);
    if (this.manual && frame.timestamp_ms >= this.manual.startMs)
      this.writeManualFrame(this.manual, frame);
  }
  writeManualFrame(manual, frame) {
    if (frame.timestamp_ms < manual.lastTimestamp) return;
    if (frame.timestamp_ms > manual.lastTimestamp) {
      manual.lastTimestamp = frame.timestamp_ms;
      manual.identitiesAtTimestamp.clear();
    }
    const identity = typeof frame.illuminator === 'string' ? frame.illuminator : '';
    if (manual.identitiesAtTimestamp.has(identity)) return;
    manual.identitiesAtTimestamp.add(identity);
    manual.gzip.write(`${JSON.stringify({type: 'frame', value: frame})}\n`);
  }
  addIlluminator(frame, now = Date.now()) {
    if (!frame || typeof frame.illuminator !== 'string') return;
    this.add({timestamp_ms: frame.timestamp_ms, illuminator: frame.illuminator,
      truth: {aircraft: frame.aircraft ?? null,
        delay_doppler: frame.delay_doppler ?? null},
      // Delay-Doppler matrices are deterministic products of the full-passband
      // IQ and replay profile.  Do not duplicate every large matrix once per
      // lane in the timeline; replay regenerates them.
      products: {map: null, map_summary: mapSummary(frame.map),
        detections: frame.detections ?? null, tracks: frame.tracks ?? null,
        doppler_window: frame.doppler_window ?? 'unknown',
        atsc_reference: frame.atsc_reference ?? null},
      first_input_sample: frame.first_input_sample ?? null,
      end_input_sample: frame.end_input_sample ?? null}, now);
  }
  start(file, startMs, metadata = {}) {
    if (!file || !Number.isFinite(startMs)) return;
    if (this.manual?.file === file) return;
    if (this.manual) void this.stop(this.manual.file, Date.now());
    const output = `${file}.timeline.jsonl.gz`;
    const temporary = `${output}.tmp-${process.pid}-${Date.now()}`;
    const destination = this.io.createWriteStream(temporary, {mode: 0o640});
    const gzip = zlib.createGzip({level: 6});
    gzip.pipe(destination);
    gzip.write(`${JSON.stringify({type: 'header', value: {
      schema: 'vectorwarp-recording-timeline-v2', start_ms: startMs, ...metadata}})}\n`);
    const manual = {file, output, temporary, destination, gzip, startMs,
      lastTimestamp: -1, identitiesAtTimestamp: new Set(), metadata, error: null};
    const failed = error => { manual.error ||= error; };
    destination.on('error', failed);
    gzip.on('error', error => { failed(error); destination.destroy(error); });
    this.manual = manual;
    for (const frame of this.frames)
      if (frame.timestamp_ms >= startMs) this.writeManualFrame(manual, frame);
  }
  async stop(file, endMs = Date.now()) {
    const manual = this.manual;
    if (!manual || (file && manual.file !== file)) return null;
    this.manual = null;
    manual.gzip.write(`${JSON.stringify({type: 'footer', value: {end_ms: endMs}})}\n`);
    const completed = manual.destination.closed ? Promise.resolve() :
      new Promise(resolve => manual.destination.once('close', resolve));
    manual.gzip.end();
    try {
      await completed;
      if (manual.error) throw manual.error;
      await this.io.promises.rename(manual.temporary, manual.output);
      this.cache = {file: '', mtimeMs: 0, value: null};
      return manual.output;
    } catch (error) {
      try { await this.io.promises.unlink(manual.temporary); } catch (_) {}
      throw error;
    }
  }
  async write(file, startMs, endMs, metadata = {}) {
    if (!file || !Number.isFinite(startMs) || !Number.isFinite(endMs)) return null;
    const reserved = this.reservations.find(item =>
      item.startMs <= startMs && item.endMs >= endMs);
    const frames = (reserved?.frames || this.frames)
      .filter(frame => frame.timestamp_ms >= startMs && frame.timestamp_ms <= endMs);
    const value = {schema: 'vectorwarp-recording-timeline-v2', start_ms: startMs,
      end_ms: endMs, ...metadata, frames};
    const output = `${file}.timeline.json`, temporary = `${output}.tmp-${process.pid}`;
    await this.io.promises.writeFile(temporary, `${JSON.stringify(value)}\n`, {mode: 0o640});
    await this.io.promises.rename(temporary, output);
    if (reserved) this.reservations = this.reservations.filter(item => item !== reserved);
    this.cache = {file: '', mtimeMs: 0, value: null};
    return output;
  }
  async writeReplayProfile(file, profile) {
    if (!file || profile?.schema !== 'vectorwarp-replay-profile-v2') return null;
    const output = `${file}.replay.json`;
    const temporary = `${output}.tmp-${process.pid}-${Date.now()}`;
    await this.io.promises.writeFile(temporary, `${JSON.stringify(profile)}\n`, {mode: 0o640});
    await this.io.promises.rename(temporary, output);
    return output;
  }
  loadReplayProfile(file) {
    if (!file) return null;
    try {
      const filename = `${file}.replay.json`, stat = this.io.statSync(filename);
      if (!stat.isFile() || stat.size > 4 * 1024 * 1024) return null;
      const value = JSON.parse(this.io.readFileSync(filename, 'utf8'));
      return value?.schema === 'vectorwarp-replay-profile-v2' ? value : null;
    } catch (_) { return null; }
  }
  has(file) {
    if (!file) return false;
    try { return this.io.statSync(`${file}.timeline.json`).isFile(); } catch (_) {}
    try { return this.io.statSync(`${file}.timeline.jsonl.gz`).isFile(); } catch (_) {}
    return false;
  }
  load(file) {
    let filename = `${file}.timeline.json`;
    try {
      if (!this.io.existsSync(filename)) filename = `${file}.timeline.jsonl.gz`;
      const stat = this.io.statSync(filename);
      if (stat.size > 256 * 1024 * 1024) return null;
      if (this.cache.file === filename && this.cache.mtimeMs === stat.mtimeMs) return this.cache.value;
      let value;
      if (filename.endsWith('.gz')) {
        const text = zlib.gunzipSync(this.io.readFileSync(filename),
          {maxOutputLength: 512 * 1024 * 1024}).toString('utf8');
        value = {frames: []};
        for (const line of text.trim().split('\n')) {
          const entry = JSON.parse(line);
          if (entry.type === 'header') Object.assign(value, entry.value);
          else if (entry.type === 'frame') value.frames.push(entry.value);
          else if (entry.type === 'footer') Object.assign(value, entry.value);
        }
      } else value = JSON.parse(this.io.readFileSync(filename, 'utf8'));
      if (!['vectorwarp-recording-timeline-v1', 'vectorwarp-recording-timeline-v2'].includes(value?.schema) ||
          !Array.isArray(value.frames)) return null;
      this.cache = {file: filename, mtimeMs: stat.mtimeMs, value}; return value;
    } catch (_) { return null; }
  }
  at(file, positionSamples, sampleRate, kind, illuminator) {
    const timeline = this.load(file); if (!timeline || !(sampleRate > 0)) return null;
    const timestamp = timeline.start_ms + positionSamples * 1000 / sampleRate;
    let selected = null;
    for (const frame of timeline.frames)
      if (frame.timestamp_ms <= timestamp &&
          (illuminator === undefined || frame.illuminator === illuminator) &&
          (!selected || frame.timestamp_ms > selected.timestamp_ms)) selected = frame;
    if (!selected) return null;
    if (illuminator !== undefined) {
      if (kind === 'aircraft' || kind === 'delay_doppler') return selected.truth?.[kind] ?? null;
      return selected.products?.[kind] ?? null;
    }
    return selected[kind] ?? null;
  }
}
module.exports = {RecordingTimeline, eventTruthAt};
