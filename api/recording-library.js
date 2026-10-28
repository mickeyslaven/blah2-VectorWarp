'use strict';

const fs = require('fs');
const fsp = fs.promises;
const path = require('path');

const RECORDING_EXTENSIONS = new Set(['.blah2iq', '.mchq']);
const SIDECAR_SUFFIXES = ['.admission.json', '.json'];
const DEFAULT_LIMIT = 100;
const MAX_LIMIT = 500;
const MINIMUM_DELETE_AGE_MS = 10 * 60 * 1000;

const finite = value => typeof value === 'number' && Number.isFinite(value);
const readJson = async filename => {
  try {
    const stat = await fsp.stat(filename);
    if (!stat.isFile() || stat.size > 4 * 1024 * 1024) return null;
    return JSON.parse(await fsp.readFile(filename, 'utf8'));
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
    lowSpacePolicy: value.low_space_policy === 'delete_oldest' ? 'delete_oldest' : 'stop_recording',
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
const sidecarsFor = filename => SIDECAR_SUFFIXES.map(suffix => `${filename}${suffix}`);
const cloneItems = items => items.map(item => ({...item,
  aircraft: item.aircraft.map(value => ({...value}))}));
const timestamp = (value, fallback) => {
  const parsed = typeof value === 'string' ? Date.parse(value) : Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
};

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
    this.scanPromise = null; this.scanError = null;
  }
  directory() { return recordingDirectory(this.getConfig()); }
  retention() { return options(this.getConfig()); }
  async scan({activeFile = '', passes = []} = {}) {
    const directory = this.directory();
    let directoryMtime = 0;
    try { directoryMtime = (await fsp.stat(directory)).mtimeMs; } catch (_) {}
    const passKey = [...passes].map(item => `${item.pass_id}:${item.flight || ''}:${item.hex || ''}`).join('|');
    const cacheKey = `${directory}:${directoryMtime}:${path.resolve(activeFile || '')}:${passKey}`;
    if (this.cache?.key === cacheKey && this.now() - this.cache.at < 5000)
      return cloneItems(this.cache.items);
    const passMap = new Map([...passes].map(item => [item.pass_id, item]));
    let names = [];
    try { names = await fsp.readdir(directory); } catch (_) { return []; }
    const recordingNames = names.filter(name => RECORDING_EXTENSIONS.has(path.extname(name).toLowerCase()));
    const items = [];
    // Yield between bounded batches: a large archive must never block live API polling.
    for (let start = 0; start < recordingNames.length; start += 24) {
      const batch = await Promise.all(recordingNames.slice(start, start + 24).map(async name => {
        const filename = path.join(directory, name);
        let stat;
        try { stat = await fsp.stat(filename); } catch (_) { return null; }
        if (!stat.isFile()) return null;
        const [metadataValue, admissionValue] = await Promise.all([
          readJson(`${filename}.json`), readJson(`${filename}.admission.json`)]);
        const metadata = metadataValue || {};
        const admission = admissionValue || {};
        const samples = metadata.samplesPerChannel ?? metadata.samples ?? null;
        const rate = metadata.sampleRate ?? null;
        const item = {id: encodeId(name), filename: name, path: filename,
          size_bytes: stat.size, created_at: timestamp(metadata.createdAt, stat.birthtimeMs || stat.mtimeMs),
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
        return item;
      }));
      items.push(...batch.filter(Boolean));
      await new Promise(resolve => setImmediate(resolve));
    }
    items.sort((left, right) => right.created_at - left.created_at);
    this.cache = {key: cacheKey, at: this.now(), items};
    return cloneItems(items);
  }
  startScan(options) {
    if (!this.scanPromise) {
      this.scanError = null;
      this.scanPromise = this.scan(options).catch(error => {
        this.scanError = error.message;
        return [];
      }).finally(() => { this.scanPromise = null; });
    }
    return this.scanPromise;
  }
  async list({activeFile = '', passes = [], search = '', offset = 0, limit = DEFAULT_LIMIT,
      waitForIndex = true} = {}) {
    const normalized = String(search || '').trim().toLowerCase();
    const boundedOffset = Number.isInteger(offset) && offset >= 0 ? offset : 0;
    const boundedLimit = Number.isInteger(limit) ? Math.max(1, Math.min(MAX_LIMIT, limit)) : DEFAULT_LIMIT;
    const stale = !this.cache || this.now() - this.cache.at >= 5000;
    if (stale) this.startScan({activeFile, passes});
    let items = waitForIndex ? await this.startScan({activeFile, passes}) :
      cloneItems(this.cache?.items || []);
    if (normalized) items = items.filter(item => [item.title, item.filename, item.receiver,
      ...item.aircraft.flatMap(value => [value.flight, value.icao])]
      .some(value => String(value || '').toLowerCase().includes(normalized)));
    const total = items.length;
    return {schema: 'vectorwarp-recording-library-v1', directory: this.directory(),
      storage: {...storage(this.directory()), recording_bytes: items.reduce((sum, item) => sum + item.size_bytes, 0)},
      retention: this.retention(), total, offset: boundedOffset, limit: boundedLimit,
      indexing: this.scanPromise !== null, index_error: this.scanError,
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
  async cleanup({activeFile = '', force = false} = {}) {
    const retention = this.retention();
    const initial = this.canRecord();
    const lowSpaceDeletion = !initial.allowed && retention.lowSpacePolicy === 'delete_oldest';
    if (!retention.automaticCleanup && !force && !lowSpaceDeletion)
      return {enabled: false, deleted: [], ...initial};
    const now = this.now();
    const active = path.resolve(activeFile || '');
    const scanned = await this.scan({activeFile});
    const items = scanned.filter(item => !item.active && item.complete &&
      now - item.modified_at >= MINIMUM_DELETE_AGE_MS).sort((a, b) => a.created_at - b.created_at);
    let total = scanned.reduce((sum, item) => sum + item.size_bytes, 0);
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
      // The low-space opt-in removes automatic aircraft clips, never manual recordings.
      // Age/size retention remains separately controlled by automatic_cleanup.
      if (lowSpace && !expired && !oversized && item.kind !== 'automatic') continue;
      if (path.resolve(item.path) === active) continue;
      const files = [item.path, ...sidecarsFor(item.path)];
      for (const filename of files) await fsp.rm(filename, {force: true});
      this.cache = null;
      total = Math.max(0, total - item.size_bytes);
      deleted.push({id: item.id, filename: item.filename, size_bytes: item.size_bytes,
        reason: expired ? 'age' : oversized ? 'storage-limit' : 'free-space-floor'});
      current = storage(this.directory());
      if (lowSpace && current.free_bytes !== null && current.free_bytes >= minFree &&
          !expired && !oversized) break;
    }
    return {enabled: true, deleted, capacity_bytes: current.capacity_bytes,
      free_bytes: current.free_bytes, recording_bytes: total};
  }
  async prepareForRecording({activeFile = ''} = {}) {
    let status = this.canRecord();
    if (!status.allowed && this.retention().lowSpacePolicy === 'delete_oldest') {
      const cleanup = await this.cleanup({activeFile});
      status = this.canRecord();
      return {...status, deleted: cleanup.deleted || []};
    }
    return status;
  }
}

module.exports = {RecordingLibrary, recordingDirectory, options, encodeId, decodeId};
