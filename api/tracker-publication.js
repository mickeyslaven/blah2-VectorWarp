'use strict';

const fs = require('fs');

const finite = value => typeof value === 'number' && Number.isFinite(value);

function publicationSettings(config) {
  const tracker = config.process?.tracker || {};
  const settings = tracker.publication || {};
  return {
    mode: settings.mode || 'native',
    eventFile: settings.event_file || '',
    staleAfterMs: finite(settings.stale_after_s) ? settings.stale_after_s * 1000 : 3_000,
    nativeMinAssociations: Number.isInteger(settings.native_min_associations) ?
      settings.native_min_associations : 5,
    delayTolerance: finite(settings.delay_tolerance) ? settings.delay_tolerance : 2,
    dopplerTolerance: finite(settings.doppler_tolerance) ? settings.doppler_tolerance : 5
  };
}

function parseObject(value) {
  if (!value) return null;
  try {
    const parsed = typeof value === 'string' ? JSON.parse(value) : value;
    return parsed && !Array.isArray(parsed) && typeof parsed === 'object' ? parsed : null;
  } catch (_) { return null; }
}

function recentDetections(event) {
  return Array.isArray(event?.detections) ? event.detections.filter(item =>
    item && finite(item.delay) && finite(item.doppler)) : [];
}

function externalTrack(event) {
  const detections = recentDetections(event);
  const latest = detections[detections.length - 1];
  if (!event || typeof event.track_id !== 'string' || !event.track_id ||
      !finite(event.timestamp) || !latest) return null;
  const length = Number.isInteger(event.length) && event.length > 0 ? event.length : detections.length;
  return {
    id: event.track_id,
    state: 'ACTIVE',
    delay: latest.delay,
    doppler: latest.doppler,
    acceleration: 0,
    n: length,
    associated_delay: detections.map(item => item.delay),
    associated_doppler: detections.map(item => item.doppler),
    associated_state: detections.map(() => 'ACTIVE'),
    source: 'unassisted',
    confidence: 'primary',
    is_anomalous: event.is_anomalous === true,
    max_velocity_ms: finite(event.max_velocity_ms) ? event.max_velocity_ms : 0,
    anomaly_types: Array.isArray(event.anomaly_types) ? event.anomaly_types : []
  };
}

class TrackerPublication {
  constructor(config, {now = Date.now} = {}) {
    this.settings = publicationSettings(config);
    this.now = now;
    this.native = '';
    this.externalTimestamp = null;
    this.external = new Map();
    this.lastExternalAt = null;
    this.lastInputAt = null;
    this.inputTimestamp = null;
    this.counters = {events: 0, invalid: 0, frames: 0};
    this.lastFusion = {nativeSupplements: 0, nativeDeduplicated: 0,
      fallbackActive: false};
  }

  acceptNative(frame) { this.native = frame; }

  acceptInput(frame, connected = true) {
    if (!connected) return;
    const value = parseObject(frame);
    if (!finite(value?.timestamp)) return;
    this.inputTimestamp = value.timestamp;
    this.lastInputAt = this.now();
  }

  acceptExternal(value) {
    const event = parseObject(value);
    const track = externalTrack(event);
    if (!track) { this.counters.invalid += 1; return false; }
    if (event.timestamp !== this.externalTimestamp) {
      this.externalTimestamp = event.timestamp;
      this.external.clear();
      this.counters.frames += 1;
    }
    this.external.set(track.id, track);
    this.lastExternalAt = this.now();
    this.counters.events += 1;
    return true;
  }

  externalFresh() {
    return this.lastExternalAt !== null &&
      this.now() - this.lastExternalAt <= this.settings.staleAfterMs && this.external.size > 0;
  }

  inputFresh() {
    return this.lastInputAt !== null && this.now() - this.lastInputAt <= this.settings.staleAfterMs;
  }

  nativeFrame() { return parseObject(this.native); }

  nativeOnly() { return this.native; }

  frame() {
    const native = this.nativeFrame();
    if (this.settings.mode === 'native') return this.native;
    if (!this.inputFresh()) {
      this.lastFusion = {nativeSupplements: 0, nativeDeduplicated: 0,
        fallbackActive: true};
      return this.native;
    }
    const primary = this.externalFresh() ? [...this.external.values()] : [];
    if (this.settings.mode === 'external') return JSON.stringify(this.buildFrame(primary, [], native));
    const supplements = [];
    let deduplicated = 0;
    for (const candidate of Array.isArray(native?.data) ? native.data : []) {
      if (!candidate || !finite(candidate.delay) || !finite(candidate.doppler) ||
          !Number.isInteger(candidate.n) || candidate.n < this.settings.nativeMinAssociations) continue;
      const duplicate = primary.some(track =>
        Math.abs(track.delay - candidate.delay) <= this.settings.delayTolerance &&
        Math.abs(track.doppler - candidate.doppler) <= this.settings.dopplerTolerance);
      if (duplicate) { deduplicated += 1; continue; }
      supplements.push({...candidate, id: `native-${candidate.id}`, source: 'native',
        confidence: 'supplement'});
    }
    this.lastFusion = {nativeSupplements: supplements.length,
      nativeDeduplicated: deduplicated, fallbackActive: false};
    return JSON.stringify(this.buildFrame(primary, supplements, native));
  }

  buildFrame(primary, supplements, native) {
    const data = [...primary, ...supplements];
    const count = state => data.filter(track => track.state === state).length;
    return {timestamp: this.externalTimestamp, n: data.length,
      nTentative: count('TENTATIVE'), nAssociated: count('ASSOCIATED'),
      nActive: count('ACTIVE'), nCoasting: count('COASTING'), data,
      publication: {mode: this.settings.mode, primary: 'unassisted',
        unassisted: primary.length, nativeSupplement: supplements.length,
        nativeTimestamp: native?.timestamp ?? null}};
  }

  status() {
    return {mode: this.settings.mode, eventFile: this.settings.eventFile,
      inputFresh: this.inputFresh(), externalFresh: this.externalFresh(),
      lastInputAt: this.lastInputAt, inputTimestamp: this.inputTimestamp,
      lastExternalAt: this.lastExternalAt,
      externalTimestamp: this.externalTimestamp, externalTracks: this.external.size,
      nativeAvailable: this.nativeFrame() !== null, settings: {...this.settings},
      counters: {...this.counters}, fusion: {...this.lastFusion}};
  }
}

class JsonlTailer {
  constructor(filename, onEvent, {fsModule = fs, schedule = setInterval,
    cancel = clearInterval, intervalMs = 100, maxReadBytes = 1024 * 1024,
    maxLineBytes = 1024 * 1024} = {}) {
    this.filename = filename;
    this.onEvent = onEvent;
    this.fs = fsModule;
    this.maxReadBytes = maxReadBytes;
    this.maxLineBytes = maxLineBytes;
    this.offset = 0;
    this.identity = null;
    this.buffer = '';
    this.active = false;
    this.closed = false;
    this.cancel = cancel;
    this.error = null;
    this.counters = {reads: 0, bytes: 0, lines: 0, invalid: 0, resets: 0};
    this.timer = schedule(() => this.poll(), intervalMs);
    this.timer.unref?.();
  }

  async poll() {
    if (this.active || this.closed || !this.filename) return;
    this.active = true;
    try {
      const stat = await this.fs.promises.stat(this.filename);
      const identity = `${stat.dev}:${stat.ino}`;
      if (this.identity === null) {
        this.identity = identity;
        this.offset = stat.size;
        this.buffer = '';
        return;
      }
      if (identity !== this.identity || stat.size < this.offset) {
        this.identity = identity;
        this.offset = 0;
        this.buffer = '';
        this.counters.resets += 1;
      }
      if (stat.size <= this.offset) return;
      const length = Math.min(stat.size - this.offset, this.maxReadBytes);
      const handle = await this.fs.promises.open(this.filename, 'r');
      try {
        const bytes = Buffer.alloc(length);
        const result = await handle.read(bytes, 0, length, this.offset);
        this.offset += result.bytesRead;
        this.counters.reads += 1;
        this.counters.bytes += result.bytesRead;
        this.consume(bytes.subarray(0, result.bytesRead).toString('utf8'));
      } finally { await handle.close(); }
    } catch (error) {
      if (error.code !== 'ENOENT') this.error = error.message;
      else this.error = 'event file is not available';
    } finally { this.active = false; }
  }

  consume(chunk) {
    this.buffer += chunk;
    if (Buffer.byteLength(this.buffer, 'utf8') > this.maxLineBytes) {
      this.buffer = '';
      this.counters.invalid += 1;
      this.error = 'external tracker event exceeds limit';
      return;
    }
    const lines = this.buffer.split('\n');
    this.buffer = lines.pop();
    for (const line of lines) {
      if (!line.trim()) continue;
      try {
        const value = JSON.parse(line);
        if (this.onEvent(value) === false) this.counters.invalid += 1;
        else this.counters.lines += 1;
        this.error = null;
      } catch (error) {
        this.counters.invalid += 1;
        this.error = `invalid external tracker event: ${error.message}`;
      }
    }
  }

  status() { return {filename: this.filename, active: this.active, error: this.error,
    ...this.counters}; }
  close() { this.closed = true; if (this.timer) this.cancel(this.timer); }
}

function createTrackerPublication(config, dependencies = {}) {
  const publication = new TrackerPublication(config, dependencies);
  const tailer = publication.settings.mode === 'native' || !publication.settings.eventFile ? null :
    new JsonlTailer(publication.settings.eventFile,
      event => publication.acceptExternal(event), dependencies.tailer || {});
  return {
    acceptNative: frame => publication.acceptNative(frame),
    acceptInput: (frame, connected) => publication.acceptInput(frame, connected),
    frame: () => publication.frame(), nativeOnly: () => publication.nativeOnly(),
    status: () => ({...publication.status(), tailer: tailer?.status() || null}),
    close() { tailer?.close(); }
  };
}

module.exports = {TrackerPublication, JsonlTailer, createTrackerPublication,
  publicationSettings, externalTrack};
