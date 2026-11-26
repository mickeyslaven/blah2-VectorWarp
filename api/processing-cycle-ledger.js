'use strict';

// Durable observation of the canonical Timing owner. Acquisition never waits
// for this writer. Only a fully fsynced checkpoint is eligible for offline export.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {exactUint64} = require('./causal-funnel-contract');
const UINT64_MAX = (1n << 64n) - 1n;
const COUNTERS = ['acceptedSamples', 'droppedSamples', 'discardedSamples'];
const OUTCOMES = new Set(['completed', 'warmup', 'rejected', 'drain', 'final']);
const object = value => value && typeof value === 'object' && !Array.isArray(value);
function requireValue(condition, message) { if (!condition) throw new Error(message); }
function fields(value, names) {
  requireValue(object(value) && Object.keys(value).length === names.length &&
    names.every(name => Object.hasOwn(value, name)), 'Unexpected cycle evidence fields');
}
function uint(value) {
  requireValue(exactUint64(value), 'Exact uint64 decimal string required');
  return BigInt(value);
}
function capture(value, previous) {
  fields(value, ['available', 'epoch', 'discontinuities', 'requalifications', 'retiredCpis', 'retiredSamples', 'lanes']);
  requireValue(typeof value.available === 'boolean' && Array.isArray(value.lanes) &&
    value.lanes.length > 0 && value.lanes.length <= 16, 'Invalid capture lanes');
  for (const key of ['epoch', 'discontinuities', 'requalifications', 'retiredCpis', 'retiredSamples']) uint(value[key]);
  if (previous) {
    requireValue(value.epoch === previous.epoch && value.lanes.length === previous.lanes.length,
      'Capture epoch or lane reset');
    for (const key of ['discontinuities', 'requalifications', 'retiredCpis', 'retiredSamples'])
      requireValue(uint(value[key]) >= uint(previous[key]), 'Capture counter reset');
  }
  const totals = Object.fromEntries(COUNTERS.map(key => [key, 0n]));
  value.lanes.forEach((lane, index) => {
    fields(lane, COUNTERS);
    for (const key of COUNTERS) {
      const current = uint(lane[key]);
      totals[key] += current;
      requireValue(totals[key] <= UINT64_MAX, 'Capture counter overflow');
      if (previous) requireValue(current >= uint(previous.lanes[index][key]), 'Lane counter reset');
    }
  });
}

function cycleHeader(evidence) {
  fields(evidence, ['schema', 'anchorMonoNs', 'anchorUtcNs', 'fault', 'records']);
  requireValue(evidence.schema === 'vectorwarp-processing-cycles-v1' && evidence.fault === '',
    'Native cycle evidence fault');
  const mono = uint(evidence.anchorMonoNs), utc = uint(evidence.anchorUtcNs);
  requireValue(mono > 0n && utc > 0n && Array.isArray(evidence.records) &&
    evidence.records.length <= 8, 'Invalid cycle clock or retention');
  return {mono, utc};
}
function cycleRecord(record, mono, utc) {
  fields(record, ['sequence', 'startMonoNs', 'endMonoNs', 'drainBatches', 'outcome',
    'captureBefore', 'captureAfter']);
  const sequence = uint(record.sequence), start = uint(record.startMonoNs), end = uint(record.endMonoNs);
  uint(record.drainBatches);
  requireValue(sequence > 0n && start >= mono && end > start &&
    end - mono <= UINT64_MAX - utc && OUTCOMES.has(record.outcome), 'Invalid cycle interval/outcome');
  capture(record.captureBefore);
  capture(record.captureAfter, record.captureBefore);
  return {sequence, start, end};
}
function latestCaptureEvidence(timing) {
  try {
    const evidence = timing?.cycleEvidence, {mono, utc} = cycleHeader(evidence);
    let previous, interval;
    for (const record of evidence.records) {
      const next = cycleRecord(record, mono, utc);
      requireValue(!interval || next.sequence === interval.sequence + 1n && next.start >= interval.end,
        'Unordered retained cycle batch');
      capture(record.captureBefore, previous);
      requireValue(record.captureBefore.available && record.captureAfter.available, 'Unavailable capture evidence');
      previous = record.captureAfter; interval = next;
    }
    return previous || null;
  } catch (_) { return null; }
}

class ProcessingCycleLedger {
  constructor({directory, io = fs.promises, maxBytes = 512 * 1024 * 1024,
    maxPendingRecords = 32, minimumFreeBytes = 1024 * 1024 * 1024} = {}) {
    this.directory = directory;
    this.io = io;
    this.maxBytes = maxBytes;
    this.maxPendingRecords = maxPendingRecords;
    this.minimumFreeBytes = minimumFreeBytes;
    this.queue = [];
    this.recent = new Map();
    this.sequence = 0n;
    this.bytes = 0;
    this.records = 0;
    this.committedRecords = 0;
    this.committedBytes = 0;
    this.inFlightRecords = 0;
    this.generation = 0;
    this.statusRevision = 0;
    this.hash = crypto.createHash('sha256');
    this.fault = '';
    this.closed = false;
    this.owned = false;
    if (!directory) return;
    this.pending = this.initialize().catch(error => this.fail(error));
  }
  async initialize() {
    await this.io.mkdir(this.directory, {mode: 0o700}); // Existing cohorts never resume/rebase.
    this.owned = true;
    await this.writeStatus('recording');
    this.file = await this.io.open(path.join(this.directory, 'cycles.jsonl'), 'wx', 0o600);
  }
  status() { return {enabled: !!this.directory, state: this.fault ? 'rejected' :
    this.closed ? 'complete' : 'recording', fault: this.fault, sequence: String(this.sequence)}; }
  fail(error) {
    if (!this.fault) ++this.statusRevision;
    this.fault ||= error.message || String(error);
    this.queue.length = 0;
    // The publication revision also invalidates any asynchronously staged seal.
    // Failures never resume a cohort.
    if (this.owned && !this.invalidating) {
      this.invalidating = Promise.resolve(this.pending).then(async () => {
        await this.revokeStatus();
        await this.writeStatus('rejected');
      }).catch(() => {});
    }
  }
  async revokeStatus() {
    try { await this.io.unlink(path.join(this.directory, 'status.json')); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    const directory = await this.io.open(this.directory, 'r');
    try { await directory.sync(); } finally { await directory.close(); }
  }
  async writeStatus(state, extra = {}) {
    const revision = this.statusRevision;
    const temporary = path.join(this.directory, 'status.pending');
    const staged = path.join(this.directory, 'status.ready');
    const file = await this.io.open(temporary, 'wx', 0o600);
    try {
      await file.writeFile(JSON.stringify({schema: 'vectorwarp-cycle-ledger-v1', state,
        fault: this.fault, ...extra}) + '\n');
      await file.sync();
    } finally { await file.close(); }
    // The asynchronous rename is preparation only: status.ready is never an
    // export authority. A fault may arrive during any awaited preparation.
    await this.io.rename(temporary, staged);
    if (revision !== this.statusRevision || (this.fault && state !== 'rejected')) {
      await this.io.unlink(staged);
      return;
    }
    // No event-loop turn can interleave the revision check and publication.
    // Only this metadata syscall is synchronous, in the API observer; native
    // acquisition and its bounded best-effort timing transport never wait here.
    fs.renameSync(staged, path.join(this.directory, 'status.json'));
    const directory = await this.io.open(this.directory, 'r');
    try { await directory.sync(); } finally { await directory.close(); }
  }
  accept(timing) {
    if (!this.directory || this.fault || this.closed) return false;
    try {
      const evidence = timing?.cycleEvidence;
      const {mono, utc} = cycleHeader(evidence);
      const anchor = `${mono}:${utc}`;
      requireValue(!this.anchor || this.anchor === anchor, 'Processor clock/session reset');
      this.anchor = anchor;
      this.anchors = {anchorMonoNs: evidence.anchorMonoNs, anchorUtcNs: evidence.anchorUtcNs};
      let batchSequence;
      for (const record of evidence.records) {
        const {sequence, start, end} = cycleRecord(record, mono, utc);
        requireValue(batchSequence === undefined || sequence === batchSequence + 1n,
          'Gap inside retained cycle batch');
        batchSequence = sequence;
        const line = JSON.stringify(record) + '\n';
        if (sequence <= this.sequence) {
          requireValue(this.recent.get(String(sequence)) === line, 'Changed or stale duplicate cycle');
          continue;
        }
        requireValue(sequence === this.sequence + 1n, 'Missing processing cycle');
        requireValue(this.lastEnd === undefined || start >= this.lastEnd, 'Processing clock regression/overlap');
        capture(record.captureBefore, this.lastCapture);
        requireValue(this.queue.length + this.inFlightRecords < this.maxPendingRecords, 'Cycle ledger queue overflow');
        const bytes = Buffer.byteLength(line);
        requireValue(this.bytes + bytes <= this.maxBytes, 'Cycle ledger capacity exceeded');
        this.bytes += bytes;
        ++this.records;
        this.sequence = sequence;
        this.lastEnd = end;
        this.lastCapture = record.captureAfter;
        this.recent.set(String(sequence), line);
        while (this.recent.size > 8) this.recent.delete(this.recent.keys().next().value);
        this.queue.push(line);
      }
      this.schedule();
      return true;
    } catch (error) { this.fail(error); return false; }
  }
  schedule() {
    if (this.writing) return;
    this.writing = true;
    this.pending = this.pending.then(async () => {
      while (this.queue.length && !this.fault) {
        // Revoke the previous prefix before any data or disk-space operation.
        await this.revokeStatus();
        await this.writeStatus('recording');
        const disk = await this.io.statfs(this.directory, {bigint: true});
        requireValue(disk.bavail * disk.bsize >= BigInt(this.minimumFreeBytes + this.bytes),
          'Cycle ledger disk floor');
        const batch = this.queue.splice(0);
        this.inFlightRecords = batch.length;
        const lines = batch.join('');
        await this.file.writeFile(lines);
        await this.file.sync();
        this.hash.update(lines);
        this.committedRecords += batch.length;
        this.committedBytes += Buffer.byteLength(lines);
        this.inFlightRecords = 0;
        if (!this.fault) await this.writeStatus('checkpoint', this.checkpoint());
      }
    }).catch(error => this.fail(error)).finally(() => {
      this.writing = false;
      if (this.queue.length && !this.fault) this.schedule();
    });
  }
  checkpoint() {
    return {...this.anchors, generation: ++this.generation, records: this.committedRecords,
      throughSequence: String(this.committedRecords), bytes: this.committedBytes,
      sha256: this.hash.copy().digest('hex')};
  }
  async close() {
    if (!this.directory || this.closed) return this.status();
    this.closed = true;
    // No ingress can enqueue after close. Initialization and the single writer
    // drain before the seal; write/fsync failure can never leave a complete seal.
    while (this.pending) {
      const pending = this.pending;
      await pending;
      if (pending === this.pending) break;
    }
    if (this.invalidating) await this.invalidating;
    try {
      if (this.owned) await this.revokeStatus();
      if (this.file) { try { await this.file.sync(); } finally { await this.file.close(); } }
      requireValue(this.records > 0, 'Empty processing cycle ledger');
      if (this.owned) await this.writeStatus(this.fault ? 'rejected' : 'complete', this.checkpoint());
    } catch (error) {
      this.fail(error);
      // A failed directory fsync after rename must not leave a visible clean seal.
      if (this.owned) try { await this.io.unlink(path.join(this.directory, 'status.json')); } catch (_) {}
    }
    return this.status();
  }
}

module.exports = {ProcessingCycleLedger, latestCaptureEvidence};
