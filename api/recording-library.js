'use strict';

const fs = require('fs');
const path = require('path');

const RECORDING_EXTENSIONS = new Set(['.blah2iq', '.mchq']);
const SIDECAR_SUFFIXES = ['.admission.json', '.json'];
const DEFAULT_LIMIT = 100;
const MAX_LIMIT = 500;
const MINIMUM_DELETE_AGE_MS = 10 * 60 * 1000;

const finite = value => typeof value === 'number' && Number.isFinite(value);
const readJson = filename => {
  try {
    const stat = fs.statSync(filename);
    if (!stat.isFile() || stat.size > 4 * 1024 * 1024) return null;
    return JSON.parse(fs.readFileSync(filename, 'utf8'));
  } catch (_) { return null; }
};
const encodeId = relative => Buffer.from(relative, 'utf8').toString('base64url');
const decodeId = id => {
  if (typeof id !== 'string' || !/^[A-Za-z0-9_-]+$/.test(id)) return null;
  try {
    const value = Buffer.from(id, 'base64url').toString('utf8');
    return value && value === path.basename(value) && encodeId(value) === id ? value : null;
  } catch (_) { return null; }
};
const recordingDirectory = config => path.resolve(config?.save?.path || '/var/lib/vectorwarp/recordings');
const options = config => {
  const value = config?.save?.retention || {};
  return {automaticCleanup: value.automatic_cleanup === true,
    maximumAgeDays: finite(value.maximum_age_days) ? value.maximum_age_days : 0,
    maximumTotalGb: finite(value.maximum_total_gb) ? value.maximum_total_gb : 0,
    minimumFreeGb: finite(value.minimum_free_gb) ? value.minimum_free_gb : 5};
};
const bytesFromGb = value => Math.round(value * 1024 * 1024 * 1024);
const storage = directory => {
  try {
    const stat = fs.statfsSync(directory);
    return {capacity_bytes: stat.blocks * stat.bsize,
      free_bytes: stat.bavail * stat.bsize};
  } catch (_) { return {capacity_bytes: null, free_bytes: null}; }
};
const sidecarsFor = filename => SIDECAR_SUFFIXES.map(suffix => `${filename}${suffix}`)
  .filter(candidate => fs.existsSync(candidate));

function aircraft(admission, passes) {
  const captures = Object.values(admission?.captures || {});
  const result = new Map();
  for (const capture of captures) {
    const truth = Array.isArray(capture.truth_samples) ? capture.truth_samples.find(Boolean) : null;
    const passId = capture.pass_id || null;
    const known = passId ? passes.get(passId) : null;
    const icao = String(capture.icao || known?.hex || '').trim().toUpperCase();
    const flight = String(known?.flight || truth?.flight || '').trim().toUpperCase();
    const key = icao || flight || passId;
    if (!key) continue;
    result.set(key, {icao: icao || null, flight: flight || null, pass_id: passId,
      slant_range_km: finite(truth?.slant_range_km) ? truth.slant_range_km : null,
      elevation_deg: finite(truth?.elevation_deg) ? truth.elevation_deg : null,
      bearing_deg_true: finite(truth?.bearing_deg_true) ? truth.bearing_deg_true : null});
  }
  return [...result.values()];
}

function titleFor(item) {
  const names = item.aircraft.map(value => value.flight || value.icao).filter(Boolean);
  const subject = names.length ? names.join(', ') : item.kind === 'automatic' ? 'Aircraft clip' : 'Manual recording';
  return `${subject} · ${new Date(item.created_at).toLocaleString('en-US', {timeZone: 'UTC',
    year: 'numeric', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit', timeZoneName: 'short'})}`;
}

class RecordingLibrary {
  constructor(getConfig, {now = Date.now} = {}) {
    this.getConfig = getConfig; this.now = now; this.cache = null;
  }
  directory() { return recordingDirectory(this.getConfig()); }
  retention() { return options(this.getConfig()); }
  scan({activeFile = '', passes = []} = {}) {
    const directory = this.directory();
    let directoryMtime = 0;
    try { directoryMtime = fs.statSync(directory).mtimeMs; } catch (_) {}
    const passKey = [...passes].map(item => `${item.pass_id}:${item.flight || ''}:${item.hex || ''}`).join('|');
    const cacheKey = `${directory}:${directoryMtime}:${path.resolve(activeFile || '')}:${passKey}`;
    if (this.cache?.key === cacheKey && this.now() - this.cache.at < 5000)
      return this.cache.items.map(item => ({...item, aircraft: item.aircraft.map(value => ({...value}))}));
    const passMap = new Map([...passes].map(item => [item.pass_id, item]));
    let names = [];
    try { names = fs.readdirSync(directory); } catch (_) { return []; }
    const items = names.filter(name => RECORDING_EXTENSIONS.has(path.extname(name).toLowerCase()))
      .flatMap(name => {
        const filename = path.join(directory, name);
        let stat;
        try { stat = fs.statSync(filename); } catch (_) { return []; }
        if (!stat.isFile()) return [];
        const metadata = readJson(`${filename}.json`) || {};
        const admission = readJson(`${filename}.admission.json`) || {};
        const samples = metadata.samplesPerChannel ?? metadata.samples ?? null;
        const rate = metadata.sampleRate ?? null;
        const item = {id: encodeId(name), filename: name, path: filename,
          size_bytes: stat.size, created_at: metadata.createdAt || stat.birthtimeMs || stat.mtimeMs,
          modified_at: stat.mtimeMs, format: metadata.format || path.extname(name).slice(1),
          receiver: metadata.receiver || null, channels: metadata.channels ?? null,
          sample_rate_hz: rate, frequency_hz: metadata.frequency ?? null,
          duration_seconds: finite(samples) && finite(rate) && rate > 0 ? samples / rate : null,
          complete: metadata.complete !== false,
          active: path.resolve(activeFile || '') === filename,
          admission: admission.state || null,
          kind: Array.isArray(metadata.eventIds) && metadata.eventIds.length ? 'automatic' : 'manual',
          aircraft: aircraft(admission, passMap)};
        item.title = titleFor(item);
        return [item];
      }).sort((left, right) => right.created_at - left.created_at);
    this.cache = {key: cacheKey, at: this.now(), items};
    return items.map(item => ({...item, aircraft: item.aircraft.map(value => ({...value}))}));
  }
  list({activeFile = '', passes = [], search = '', offset = 0, limit = DEFAULT_LIMIT} = {}) {
    const normalized = String(search || '').trim().toLowerCase();
    const boundedOffset = Number.isInteger(offset) && offset >= 0 ? offset : 0;
    const boundedLimit = Number.isInteger(limit) ? Math.max(1, Math.min(MAX_LIMIT, limit)) : DEFAULT_LIMIT;
    let items = this.scan({activeFile, passes});
    if (normalized) items = items.filter(item => [item.title, item.filename, item.receiver,
      ...item.aircraft.flatMap(value => [value.flight, value.icao])]
      .some(value => String(value || '').toLowerCase().includes(normalized)));
    const total = items.length;
    return {schema: 'vectorwarp-recording-library-v1', directory: this.directory(),
      storage: {...storage(this.directory()), recording_bytes: items.reduce((sum, item) => sum + item.size_bytes, 0)},
      retention: this.retention(), total, offset: boundedOffset, limit: boundedLimit,
      recordings: items.slice(boundedOffset, boundedOffset + boundedLimit)};
  }
  resolve(id) {
    const name = decodeId(id);
    if (!name || !RECORDING_EXTENSIONS.has(path.extname(name).toLowerCase())) return null;
    const filename = path.resolve(this.directory(), name);
    if (path.dirname(filename) !== this.directory()) return null;
    try { return fs.statSync(filename).isFile() ? filename : null; } catch (_) { return null; }
  }
  canRecord() {
    const retention = this.retention();
    const current = storage(this.directory());
    const required = bytesFromGb(retention.minimumFreeGb);
    return {allowed: current.free_bytes === null || current.free_bytes > required,
      free_bytes: current.free_bytes, required_free_bytes: required};
  }
  cleanup({activeFile = '', force = false} = {}) {
    const retention = this.retention();
    if (!retention.automaticCleanup && !force)
      return {enabled: false, deleted: [], ...this.list({activeFile, limit: 1}).storage};
    const now = this.now();
    const active = path.resolve(activeFile || '');
    const items = this.scan({activeFile}).filter(item => !item.active && item.complete &&
      now - item.modified_at >= MINIMUM_DELETE_AGE_MS).sort((a, b) => a.created_at - b.created_at);
    let total = this.scan({activeFile}).reduce((sum, item) => sum + item.size_bytes, 0);
    let current = storage(this.directory());
    const maxBytes = retention.maximumTotalGb > 0 ? bytesFromGb(retention.maximumTotalGb) : 0;
    const minFree = bytesFromGb(retention.minimumFreeGb);
    const maxAge = retention.maximumAgeDays > 0 ? retention.maximumAgeDays * 86400000 : 0;
    const deleted = [];
    for (const item of items) {
      const expired = maxAge > 0 && now - item.created_at > maxAge;
      const oversized = maxBytes > 0 && total > maxBytes;
      const lowSpace = current.free_bytes !== null && current.free_bytes < minFree;
      if (!expired && !oversized && !lowSpace) continue;
      if (path.resolve(item.path) === active) continue;
      const files = [item.path, ...sidecarsFor(item.path)];
      for (const filename of files) fs.rmSync(filename, {force: true});
      this.cache = null;
      total = Math.max(0, total - item.size_bytes);
      deleted.push({id: item.id, filename: item.filename, size_bytes: item.size_bytes,
        reason: expired ? 'age' : oversized ? 'storage-limit' : 'free-space-floor'});
      current = storage(this.directory());
    }
    return {enabled: true, deleted, capacity_bytes: current.capacity_bytes,
      free_bytes: current.free_bytes, recording_bytes: total};
  }
}

module.exports = {RecordingLibrary, recordingDirectory, options, encodeId, decodeId};
