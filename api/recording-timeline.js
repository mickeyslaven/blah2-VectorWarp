'use strict';
const fs = require('fs');
const zlib = require('zlib');

class RecordingTimeline {
  constructor({maxAgeMs = 30000, io = fs} = {}) {
    this.maxAgeMs = maxAgeMs; this.io = io; this.frames = [];
    this.cache = {file: '', mtimeMs: 0, value: null};
    this.manual = null;
  }
  add(frame, now = Date.now()) {
    if (!Number.isSafeInteger(frame?.timestamp_ms) || frame.timestamp_ms < 0) return;
    this.frames.push(frame);
    while (this.frames.length > 1 && this.frames[0].timestamp_ms < now - this.maxAgeMs)
      this.frames.shift();
    if (this.manual && frame.timestamp_ms >= this.manual.startMs &&
        frame.timestamp_ms > this.manual.lastTimestamp) {
      this.manual.lastTimestamp = frame.timestamp_ms;
      this.manual.gzip.write(`${JSON.stringify({type: 'frame', value: frame})}\n`);
    }
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
      schema: 'vectorwarp-recording-timeline-v1', start_ms: startMs, ...metadata}})}\n`);
    const manual = {file, output, temporary, destination, gzip, startMs,
      lastTimestamp: -1, metadata, error: null};
    const failed = error => { manual.error ||= error; };
    destination.on('error', failed);
    gzip.on('error', error => { failed(error); destination.destroy(error); });
    this.manual = manual;
    for (const frame of this.frames)
      if (frame.timestamp_ms >= startMs) {
        manual.lastTimestamp = frame.timestamp_ms;
        gzip.write(`${JSON.stringify({type: 'frame', value: frame})}\n`);
      }
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
    const frames = this.frames.filter(frame => frame.timestamp_ms >= startMs && frame.timestamp_ms <= endMs);
    const value = {schema: 'vectorwarp-recording-timeline-v1', start_ms: startMs,
      end_ms: endMs, ...metadata, frames};
    const output = `${file}.timeline.json`, temporary = `${output}.tmp-${process.pid}`;
    await this.io.promises.writeFile(temporary, `${JSON.stringify(value)}\n`, {mode: 0o640});
    await this.io.promises.rename(temporary, output); return output;
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
      if (value?.schema !== 'vectorwarp-recording-timeline-v1' || !Array.isArray(value.frames)) return null;
      this.cache = {file: filename, mtimeMs: stat.mtimeMs, value}; return value;
    } catch (_) { return null; }
  }
  at(file, positionSamples, sampleRate, kind) {
    const timeline = this.load(file); if (!timeline || !(sampleRate > 0)) return null;
    const timestamp = timeline.start_ms + positionSamples * 1000 / sampleRate;
    let selected = null;
    for (const frame of timeline.frames)
      if (frame.timestamp_ms <= timestamp && (!selected || frame.timestamp_ms > selected.timestamp_ms)) selected = frame;
    return selected?.[kind] ?? null;
  }
}
module.exports = {RecordingTimeline};
