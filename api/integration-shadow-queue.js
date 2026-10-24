'use strict';

// The live API only emits immutable, admitted work descriptions.  It never
// executes replay DSP: the expensive comparison belongs on a separate worker
// (normally Strix), so enabling this shadow cannot steal a live CPI deadline.
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');

const DEFAULT_DWELLS = Object.freeze([0.05, 0.1, 0.2, 0.5]);
const DEFAULT_MODES = Object.freeze([
  'production', 'coherent_beam_bank', 'noise_whitened_beam_bank', 'noncoherent',
  'physical_channels'
]);
const MAX_HOT_COMPLETED = 256;

const canonical = value => {
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) throw new TypeError('canonical shadow receipts require finite numbers');
    const bytes = Buffer.allocUnsafe(8); bytes.writeDoubleBE(value);
    return `{"$f64":"${bytes.toString('hex')}"}`;
  }
  if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
  if (value && typeof value === 'object') return `{${Object.keys(value).sort().map(key =>
    `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}`;
  return JSON.stringify(value);
};

const digestCanonical = value => crypto.createHash('sha256').update(canonical(value)).digest('hex');

function atomicWrite(filename, value, fileSystem = fs) {
  fileSystem.mkdirSync(path.dirname(filename), {recursive: true});
  const temporary = `${filename}.tmp-${process.pid}-${Date.now()}`;
  fileSystem.writeFileSync(temporary, value, {mode: 0o600});
  fileSystem.renameSync(temporary, filename);
}

function atomicWriteExclusive(filename, value, fileSystem = fs) {
  fileSystem.mkdirSync(path.dirname(filename), {recursive: true});
  const temporary = `${filename}.tmp-${process.pid}-${Date.now()}-${crypto.randomBytes(6).toString('hex')}`;
  fileSystem.writeFileSync(temporary, value, {mode: 0o600, flag: 'wx'});
  try { fileSystem.linkSync(temporary, filename); }
  finally { fileSystem.unlinkSync(temporary); }
}

function optionsFromConfig(config) {
  const input = config.process?.detection?.integration_shadow || {};
  return {
    enabled: input.enabled === true,
    queuePath: input.queue_path || '/var/lib/vectorwarp-api/integration-shadow/queue',
    resultsPath: input.results_path || '/var/lib/vectorwarp-api/integration-shadow/results',
    receiptKeyPath: input.receipt_key_path || null,
    dwellSeconds: input.dwell_seconds || [...DEFAULT_DWELLS],
    fusionModes: input.fusion_modes || [...DEFAULT_MODES],
    maxPending: input.max_pending ?? 64,
    maxWallSecondsPerClip: input.max_wall_seconds_per_clip ?? 1800
  };
}

class IntegrationShadowQueue {
  constructor(config, {fileSystem = fs, now = Date.now} = {}) {
    this.config = config;
    this.options = optionsFromConfig(config);
    this.fs = fileSystem;
    this.now = now;
    this.counters = {accepted: 0, duplicate: 0, rejected: 0, overflow: 0};
    this.lastQueuedAt = null;
    this.lastError = null;
  }

  receiptKey() {
    const filename = this.options.receiptKeyPath;
    if (!filename || !path.isAbsolute(filename)) throw new Error('integration shadow receipt key is not configured');
    const stat = this.fs.lstatSync(filename);
    if (!stat.isFile() || stat.isSymbolicLink() || (stat.mode & 0o077) !== 0 ||
        (typeof process.geteuid === 'function' && stat.uid !== process.geteuid()))
      throw new Error('integration shadow receipt key must be an owner-only regular file');
    const key = this.fs.readFileSync(filename);
    if (key.length < 32) throw new Error('integration shadow receipt key must contain at least 32 bytes');
    return key;
  }

  safeDirectory(filename) {
    this.fs.mkdirSync(filename, {recursive: true, mode: 0o700});
    const stat = this.fs.lstatSync(filename);
    if (!stat.isDirectory() || stat.isSymbolicLink() || (stat.mode & 0o022) !== 0 ||
        (typeof process.geteuid === 'function' && stat.uid !== process.geteuid()))
      throw new Error('integration shadow spool directories must be owner-controlled and not writable by group/other');
  }

  files(directory, suffix) {
    try { return this.fs.readdirSync(directory).filter(name => name.endsWith(suffix)); }
    catch (error) { if (error.code === 'ENOENT') return []; throw error; }
  }

  resultRecords() {
    return this.files(this.options.resultsPath, '.result.json').sort().map(name => {
      try {
        const result = JSON.parse(this.fs.readFileSync(path.join(this.options.resultsPath, name)));
        const expected = name.replace(/\.result\.json$/, '');
        const jobPath = path.join(this.options.queuePath, `${expected}.job.json`);
        const job = JSON.parse(this.fs.readFileSync(jobPath));
        const unsignedJob = {...job}; delete unsignedJob.job_binding_sha256;
        const unsignedResult = {...result}; delete unsignedResult.result_binding_sha256;
        delete unsignedResult.result_authentication;
        const authenticatedResult = {...result};
        const authentication = authenticatedResult.result_authentication;
        delete authenticatedResult.result_authentication;
        const key = this.receiptKey();
        const expectedTag = crypto.createHmac('sha256', key).update(canonical(authenticatedResult)).digest('hex');
        const suppliedTag = authentication?.tag;
        if (result.schema !== 'vectorwarp-integration-shadow-result-v1' ||
            result.job_id !== expected || result.state !== 'complete' ||
            result.shadow_only !== true || result.production_unchanged !== true ||
            job.job_id !== expected || digestCanonical(job.identity) !== expected ||
            digestCanonical(unsignedJob) !== job.job_binding_sha256 ||
            digestCanonical(unsignedResult) !== result.result_binding_sha256 ||
            authentication?.algorithm !== 'hmac-sha256' ||
            authentication?.key_id !== crypto.createHash('sha256').update(key).digest('hex') ||
            !/^[a-f0-9]{64}$/.test(suppliedTag || '') ||
            !crypto.timingSafeEqual(Buffer.from(expectedTag, 'hex'), Buffer.from(suppliedTag, 'hex')) ||
            result.job_binding_sha256 !== job.job_binding_sha256 ||
            result.input?.recording_sha256 !== job.identity?.recording_sha256 ||
            result.input?.native_sidecar_sha256 !== job.identity?.native_sidecar_sha256 ||
            result.input?.admission_sha256 !== job.identity?.admission_sha256 ||
            result.input?.config_sha256 !== job.identity?.config_sha256 ||
            !result.arms || typeof result.arms !== 'object' ||
            !result.summary || result.summary.selection_performed !== false)
          throw new Error('result receipt contract failed');
        return {name, valid: true, result};
      } catch (error) { return {name, valid: false, error: error.message}; }
    });
  }

  compactCompleted() {
    const valid = this.resultRecords().filter(row => row.valid);
    if (valid.length <= MAX_HOT_COMPLETED) return 0;
    const archive = path.join(this.options.resultsPath, 'archive');
    const jobArchive = path.join(archive, 'jobs');
    const resultArchive = path.join(archive, 'results');
    this.fs.mkdirSync(jobArchive, {recursive: true});
    this.fs.mkdirSync(resultArchive, {recursive: true});
    let moved = 0;
    for (const row of valid.slice(0, valid.length - MAX_HOT_COMPLETED)) {
      const id = row.result.job_id;
      const sourceJob = path.join(this.options.queuePath, `${id}.job.json`);
      const sourceResult = path.join(this.options.resultsPath, row.name);
      this.fs.renameSync(sourceJob, path.join(jobArchive, `${id}.job.json`));
      this.fs.renameSync(sourceResult, path.join(resultArchive, row.name));
      ++moved;
    }
    return moved;
  }

  reconcileSlots() {
    const completed = new Set(this.resultRecords().filter(row => row.valid)
      .map(row => row.result.job_id));
    for (const name of this.files(this.options.queuePath, '.slot')) {
      const filename = path.join(this.options.queuePath, name);
      try {
        const slot = JSON.parse(this.fs.readFileSync(filename));
        const jobExists = this.fs.existsSync(path.join(this.options.queuePath, `${slot.job_id}.job.json`));
        let ownerAlive = true;
        if (slot.owner_host === os.hostname() && Number.isInteger(slot.owner_pid) && slot.owner_pid > 1) {
          try { process.kill(slot.owner_pid, 0); }
          catch (error) { if (error.code === 'ESRCH') ownerAlive = false; else throw error; }
        }
        const oldOrphan = !jobExists && slot.owner_host === os.hostname() && !ownerAlive &&
          Date.now() - this.fs.statSync(filename).mtimeMs > 60000;
        if (completed.has(slot.job_id) || oldOrphan) this.fs.unlinkSync(filename);
      } catch (_) { /* Invalid slots remain visibly capacity-consuming for operator review. */ }
    }
  }

  reserveSlot(jobId) {
    for (let index = 0; index < this.options.maxPending; ++index) {
      const filename = path.join(this.options.queuePath, `pending-${index}.slot`);
      try {
        this.fs.writeFileSync(filename, `${JSON.stringify({job_id: jobId,
          owner_pid: process.pid, owner_host: os.hostname(),
          owner_nonce: crypto.randomBytes(16).toString('hex'), created_at: Date.now()})}\n`,
        {mode: 0o600, flag: 'wx'});
        return filename;
      } catch (error) { if (error.code !== 'EEXIST') throw error; }
    }
    return null;
  }

  status() {
    if (this.options.enabled) {
      try {
        this.safeDirectory(this.options.queuePath);
        this.safeDirectory(this.options.resultsPath);
        this.receiptKey();
      } catch (error) {
        return {schema: 'vectorwarp-integration-shadow-status-v1', enabled: true,
          execution: 'external-worker-only', authoritative_output: false,
          production_path_changed: false, state: 'configuration-error', error: error.message,
          queued: 0, pending: 0, completed: 0, invalid_results: 0,
          counters: {...this.counters}};
      }
    }
    this.reconcileSlots();
    const queued = this.files(this.options.queuePath, '.job.json');
    const records = this.resultRecords();
    const completed = new Set(records.filter(row => row.valid).map(row => row.result.job_id));
    const pending = queued.filter(name => !completed.has(name.replace(/\.job\.json$/, '')));
    const recent = records.slice(-12).map(row => {
      if (row.valid) {
        const result = row.result;
        return {job_id: result.job_id, state: result.state, completed_at: result.completed_at,
          timing: result.timing, winner: result.summary?.winner || null,
          production_unchanged: result.production_unchanged === true};
      }
      return {file: row.name, state: 'invalid', error: row.error};
    });
    return {schema: 'vectorwarp-integration-shadow-status-v1', enabled: this.options.enabled,
      execution: 'external-worker-only', authoritative_output: false,
      production_path_changed: false, queue_path: this.options.queuePath,
      results_path: this.options.resultsPath, dwell_seconds: this.options.dwellSeconds,
      fusion_modes: this.options.fusionModes, max_wall_seconds_per_clip:
        this.options.maxWallSecondsPerClip, queued: queued.length, pending: pending.length,
      completed: completed.size, invalid_results: records.length - completed.size,
      reserved_slots: this.files(this.options.queuePath, '.slot').length,
      hot_completed_limit: MAX_HOT_COMPLETED,
      counters: {...this.counters}, last_queued_at: this.lastQueuedAt,
      last_error: this.lastError, recent};
  }

  accept(file, artifact, revision) {
    if (!this.options.enabled) return {accepted: false, reason: 'disabled'};
    try {
      const captures = Object.entries(artifact?.captures || {}).filter(([, item]) =>
        item?.valid === true && item?.admission_state === 'admitted' &&
        Array.isArray(item.processor_cpis) && item.processor_cpis.length >= 3);
      if (artifact?.state !== 'admitted' || artifact?.integrity?.valid !== true ||
          !/^[a-f0-9]{64}$/i.test(artifact?.integrity?.data_sha256 || '') ||
          !/^[a-f0-9]{64}$/i.test(artifact?.integrity?.sidecar_sha256 || '') ||
          typeof file !== 'string' || !path.isAbsolute(file) || !captures.length) {
        ++this.counters.rejected;
        return {accepted: false, reason: 'not-strictly-admitted'};
      }
      const admissionSha256 = digestCanonical(artifact);
      const configSha256 = digestCanonical(this.config);
      const receiptKeyId = crypto.createHash('sha256').update(this.receiptKey()).digest('hex');
      const identity = {recording: file, recording_sha256: artifact.integrity.data_sha256,
        native_sidecar_sha256: artifact.integrity.sidecar_sha256,
        admission_sha256: admissionSha256, config_sha256: configSha256,
        receipt_key_id: receiptKeyId,
        producer_host: os.hostname(),
        revision, config_epoch: artifact.config_epoch, receiver_epoch: artifact.receiver_epoch};
      const jobId = crypto.createHash('sha256').update(canonical(identity)).digest('hex');
      const output = path.join(this.options.queuePath, `${jobId}.job.json`);
      this.safeDirectory(this.options.queuePath);
      this.safeDirectory(this.options.resultsPath);
      this.reconcileSlots();
      this.compactCompleted();
      if (this.fs.existsSync(output)) {
        ++this.counters.duplicate;
        return {accepted: false, reason: 'duplicate', job_id: jobId};
      }
      const slot = this.reserveSlot(jobId);
      if (!slot) {
        ++this.counters.overflow;
        this.lastError = 'integration shadow queue reached its configured bound';
        return {accepted: false, reason: 'queue-full'};
      }
      let committed = false;
      try {
        const job = {schema: 'vectorwarp-integration-shadow-job-v1', job_id: jobId,
          created_at: new Date(this.now()).toISOString(), recording: file,
          admission_sidecar: `${file}.admission.json`, identity,
          experiment: {same_iq: true, shadow_only: true, dwell_seconds: this.options.dwellSeconds,
            fusion_modes: this.options.fusionModes,
            receipt_key_id: receiptKeyId,
            max_wall_seconds_per_clip: this.options.maxWallSecondsPerClip,
            require_production_arm: true, physical_channel_reference_policy: 'leave-one-out'},
          captures: captures.map(([captureId, item]) => ({capture_id: captureId,
            processor_cpis: item.processor_cpis.length}))};
        job.job_binding_sha256 = digestCanonical(job);
        atomicWriteExclusive(output, `${JSON.stringify(job, null, 2)}\n`, this.fs);
        committed = true;
      } finally {
        if (!committed) try { this.fs.unlinkSync(slot); } catch (_) {}
      }
      ++this.counters.accepted;
      this.lastQueuedAt = this.now();
      this.lastError = null;
      return {accepted: true, job_id: jobId, path: output};
    } catch (error) {
      ++this.counters.rejected; this.lastError = error.message;
      return {accepted: false, reason: 'write-failed', error: error.message};
    }
  }
}

module.exports = {IntegrationShadowQueue, atomicWrite, atomicWriteExclusive, canonical,
  digestCanonical, optionsFromConfig,
  DEFAULT_DWELLS, DEFAULT_MODES};
