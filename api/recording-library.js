'use strict';

const fs = require('fs');
const fsp = fs.promises;
const path = require('path');

const RECORDING_EXTENSIONS = new Set(['.blah2iq', '.mchq']);
const SIDECAR_SUFFIXES = ['.admission.json', '.json'];
const DEFAULT_LIMIT = 100;
const MAX_LIMIT = 500;
const METADATA_BATCH = 32;
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
const metadataKey = item => `${item.filename}\0${item.modified_at}`;
const cloneItems = items => items.map(item => ({...item,
  aircraft: item.aircraft.map(value => ({...value}))}));
const timestamp = (value, fallback) => {
  const parsed = typeof value === 'string' ? Date.parse(value) : Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
};

function aircraft(admission, passes = new Map()) {
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
  constructor(getConfig, {now = Date.now, environment = process.env} = {}) {
    this.getConfig = getConfig; this.now = now; this.cache = null;
    this.environment = environment;
    this.scanPromise = null; this.scanError = null; this.metadataCache = new Map();
    this.metadataIndexPromise = null; this.metadataIndexKey = null;
  }
  directory() { return recordingDirectory(this.getConfig()); }
  root() {
    if (this.environment.VECTORWARP_RECORDING_ROOT)
      return path.resolve(this.environment.VECTORWARP_RECORDING_ROOT);
    if (this.environment.VECTORWARP_LINUX_SERVICE === '1')
      return '/var/lib/vectorwarp/recordings';
    if (this.environment.VECTORWARP_MACOS_STATE)
      return path.resolve(this.environment.VECTORWARP_MACOS_STATE, 'recordings');
    return this.directory();
  }
  retention() { return options(this.getConfig()); }
  async scan({activeFile = '', passes = []} = {}) {
    const directory = this.directory();
    let directoryMtime = 0;
    try { directoryMtime = (await fsp.stat(directory)).mtimeMs; } catch (_) {}
    const cacheKey = `${directory}:${directoryMtime}`;
    if (this.cache?.key === cacheKey) return this.cache.items;
    let names = [];
    try { names = await fsp.readdir(directory); } catch (_) { return []; }
    const recordingNames = names.filter(name => RECORDING_EXTENSIONS.has(path.extname(name).toLowerCase()));
    const items = [];
    // Yield between bounded batches: a large archive must never block live API polling.
    for (let start = 0; start < recordingNames.length; start += 64) {
      const batch = await Promise.all(recordingNames.slice(start, start + 64).map(async name => {
        const filename = path.join(directory, name);
        let stat;
        try { stat = await fsp.stat(filename); } catch (_) { return null; }
        if (!stat.isFile()) return null;
        const item = {id: encodeId(name), filename: name, path: filename,
          size_bytes: stat.size, created_at: stat.birthtimeMs || stat.mtimeMs,
          modified_at: stat.mtimeMs, format: path.extname(name).slice(1),
          receiver: null, channels: null, sample_rate_hz: null, frequency_hz: null,
          duration_seconds: null, complete: true,
          active: false,
          admission: null, kind: path.extname(name).toLowerCase() === '.mchq' ? 'automatic' : 'manual',
          aircraft: []};
        item.title = titleFor(item);
        return item;
      }));
      items.push(...batch.filter(Boolean));
      await new Promise(resolve => setImmediate(resolve));
    }
    items.sort((left, right) => right.created_at - left.created_at);
    this.cache = {key: cacheKey, at: this.now(), items};
    this.pruneMetadata(recordingNames);
    return items;
  }
  pruneMetadata(recordingNames) {
    const names = new Set(recordingNames);
    for (const key of this.metadataCache.keys())
      if (!names.has(key.split('\0', 1)[0])) this.metadataCache.delete(key);
  }
  async metadata(item) {
    const key = metadataKey(item);
    if (this.metadataCache.has(key)) return this.metadataCache.get(key);
    const [metadataValue, admissionValue] = await Promise.all([
      readJson(`${item.path}.json`), readJson(`${item.path}.admission.json`)]);
    const metadata = metadataValue || {};
    const admission = admissionValue || {};
    const samples = metadata.samplesPerChannel ?? metadata.samples ?? null;
    const rate = metadata.sampleRate ?? null;
    const enriched = {...item,
      created_at: timestamp(metadata.createdAt, item.created_at),
      format: metadata.format || item.format, receiver: metadata.receiver || null,
      channels: metadata.channels ?? null, sample_rate_hz: rate,
      frequency_hz: metadata.frequency ?? null,
      duration_seconds: finite(samples) && finite(rate) && rate > 0 ? samples / rate : null,
      complete: metadata.complete !== false, admission: admission.state || null,
      kind: Array.isArray(metadata.eventIds) && metadata.eventIds.length ? 'automatic' : item.kind,
      aircraft: aircraft(admission)};
    enriched.title = titleFor(enriched);
    this.metadataCache.set(key, enriched);
    return enriched;
  }
  async enrich(item, passMap, activeFile = '') {
    const base = await this.metadata(item);
    const enriched = {...base, active: path.resolve(activeFile || '') === base.path,
      aircraft: base.aircraft.map(value => {
      const known = value.pass_id ? passMap.get(value.pass_id) : null;
      return {...value, icao: value.icao || known?.hex || null,
        flight: value.flight || known?.flight || null};
    })};
    enriched.title = titleFor(enriched);
    return cloneItems([enriched])[0];
  }
  startMetadataIndex(items) {
    const key = this.cache?.key || '';
    if (this.metadataIndexKey === key && !this.metadataIndexPromise) return null;
    if (this.metadataIndexPromise) return this.metadataIndexPromise;
    this.metadataIndexPromise = (async () => {
      for (let start = 0; start < items.length; start += METADATA_BATCH) {
        await Promise.all(items.slice(start, start + METADATA_BATCH).map(item => this.metadata(item)));
        await new Promise(resolve => setImmediate(resolve));
      }
      this.metadataIndexKey = key;
    })().catch(error => { this.scanError = error.message; }).finally(() => {
      this.metadataIndexPromise = null;
    });
    return this.metadataIndexPromise;
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
  async list({activeFile = '', passes = [], search = '', kind = '', receiver = '',
      frequency = '', since = '', sort = 'newest', offset = 0, limit = DEFAULT_LIMIT,
      waitForIndex = true} = {}) {
    const normalized = String(search || '').trim().toLowerCase();
    const boundedOffset = Number.isInteger(offset) && offset >= 0 ? offset : 0;
    const boundedLimit = Number.isInteger(limit) ? Math.max(1, Math.min(MAX_LIMIT, limit)) : DEFAULT_LIMIT;
    const stale = !this.cache || this.now() - this.cache.at >= 5000;
    if (stale) this.startScan({activeFile, passes});
    let items = waitForIndex ? await this.startScan({activeFile, passes}) :
      (this.cache?.items || []);
    const passMap = new Map([...passes].map(item => [item.pass_id, item]));
    // Opening the library must not turn into an archive-wide I/O job. Only an
    // explicit metadata-dependent query indexes every recording; a normal page
    // enriches just the visible rows below.
    const needsMetadataIndex = Boolean(normalized || kind || receiver || frequency);
    const metadataIndex = needsMetadataIndex ? this.startMetadataIndex(items) : null;
    if (metadataIndex) await metadataIndex;
    const facets = {receivers: [...new Set(items.map(item =>
      this.metadataCache.get(metadataKey(item))?.receiver).filter(Boolean))].sort(),
    frequencies: [...new Set(items.map(item =>
      this.metadataCache.get(metadataKey(item))?.frequency_hz)
      .filter(value => Number.isFinite(value)))].sort((a, b) => a - b)};
    if (normalized) items = items.filter(item => {
      const cached = this.metadataCache.get(metadataKey(item)) || item;
      return [cached.title, cached.filename, cached.receiver,
      ...cached.aircraft.flatMap(value => [value.flight, value.icao])]
      .some(value => String(value || '').toLowerCase().includes(normalized));
    });
    if (['manual', 'automatic'].includes(kind)) items = items.filter(item => {
      const cached = this.metadataCache.get(metadataKey(item)) || item;
      return cached.kind === kind;
    });
    if (receiver) items = items.filter(item => {
      const cached = this.metadataCache.get(metadataKey(item)) || item;
      return String(cached.receiver || '').toLowerCase() === String(receiver).toLowerCase();
    });
    if (frequency) items = items.filter(item => {
      const cached = this.metadataCache.get(metadataKey(item)) || item;
      return Number(cached.frequency_hz) === Number(frequency);
    });
    if (Number.isFinite(Number(since)) && Number(since) > 0)
      items = items.filter(item => item.created_at >= Number(since));
    if (sort !== 'newest') items = items.slice();
    if (sort === 'oldest') items.sort((left, right) => left.created_at - right.created_at);
    else if (sort === 'largest') items.sort((left, right) => right.size_bytes - left.size_bytes);
    else if (sort === 'smallest') items.sort((left, right) => left.size_bytes - right.size_bytes);
    const total = items.length;
    const page = await Promise.all(items.slice(boundedOffset, boundedOffset + boundedLimit)
      .map(item => this.enrich(item, passMap, activeFile)));
    return {schema: 'vectorwarp-recording-library-v1', directory: this.directory(),
      storage: {...storage(this.directory()), recording_bytes:
        (this.cache?.items || items).reduce((sum, item) => sum + item.size_bytes, 0)},
      retention: this.retention(), total, offset: boundedOffset, limit: boundedLimit,
      facets,
      indexing: this.scanPromise !== null, metadata_indexing: this.metadataIndexPromise !== null,
      index_error: this.scanError,
      recordings: page};
  }
  resolve(id) {
    const name = decodeId(id);
    if (!name || !RECORDING_EXTENSIONS.has(path.extname(name).toLowerCase())) return null;
    const filename = path.resolve(this.directory(), name);
    if (path.dirname(filename) !== this.directory()) return null;
    try { return fs.statSync(filename).isFile() ? filename : null; } catch (_) { return null; }
  }
  async directories() {
    const root = this.root();
    const result = [];
    const visit = async (directory, depth) => {
      let entries;
      try {
        await fsp.access(directory, fs.constants.R_OK | fs.constants.W_OK);
        result.push({path: `${directory.replace(/\/$/, '')}/`, selected:
          path.resolve(directory) === this.directory()});
        if (depth >= 3) return;
        entries = await fsp.readdir(directory, {withFileTypes: true});
      } catch (_) { return; }
      for (const entry of entries) {
        if (!entry.isDirectory() || entry.isSymbolicLink() || entry.name.startsWith('.')) continue;
        await visit(path.join(directory, entry.name), depth + 1);
      }
    };
    await visit(root, 0);
    return {root: `${root.replace(/\/$/, '')}/`, directories: result};
  }
  async createDirectory(name) {
    const relative = String(name || '').trim();
    if (!relative || relative.length > 160 || relative.includes('..') ||
        path.isAbsolute(relative) || !/^[A-Za-z0-9][A-Za-z0-9._ /-]*$/.test(relative))
      throw Object.assign(new Error('Choose a simple folder name inside the VectorWarp recordings directory.'), {status: 400});
    const root = this.root();
    const directory = path.resolve(root, relative);
    if (!directory.startsWith(`${path.resolve(root)}${path.sep}`))
      throw Object.assign(new Error('Recording folders must remain inside the VectorWarp recordings directory.'), {status: 400});
    await fsp.mkdir(directory, {recursive: true, mode: 0o770});
    const [realRoot, realDirectory] = await Promise.all([fsp.realpath(root), fsp.realpath(directory)]);
    if (realDirectory !== realRoot && !realDirectory.startsWith(`${realRoot}${path.sep}`))
      throw Object.assign(new Error('The selected folder resolves outside VectorWarp storage.'), {status: 400});
    await fsp.access(directory, fs.constants.R_OK | fs.constants.W_OK);
    return {path: `${directory.replace(/\/$/, '')}/`};
  }
  async remove(id, {activeFile = ''} = {}) {
    const filename = this.resolve(id);
    if (!filename) return {removed: false, reason: 'not-found'};
    if (path.resolve(activeFile || '') === filename)
      return {removed: false, reason: 'active'};
    const stat = await fsp.stat(filename);
    for (const candidate of [filename, ...sidecarsFor(filename)])
      await fsp.rm(candidate, {force: true});
    this.cache = null;
    for (const key of this.metadataCache.keys())
      if (key.startsWith(`${path.basename(filename)}:`)) this.metadataCache.delete(key);
    return {removed: true, filename: path.basename(filename), size_bytes: stat.size};
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
      const expired = maxAge > 0 && now - Math.min(item.created_at, item.modified_at) > maxAge;
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
