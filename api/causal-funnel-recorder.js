'use strict';

// Phase 1/2 collector only.  It accepts a deliberately small radar-only wire
// contract; no truth provider is imported or accepted as a constructor input.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {exactUint64} = require('./causal-funnel-contract');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const truthKey = /(?:^|_)(adsb|truth|aircraft|icao|hex|flight|callsign|latitude|longitude|altitude|position|identity)(?:$|_)/i;
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const FRAME_KEYS = new Set(['schema', 'source', 'event_time_ns', 'sequence', 'stage', 'reason', 'candidates', 'producer']);
const CANDIDATE_KEYS = new Set(['id', 'delay_km', 'doppler_hz', 'snr_db', 'bearing_candidates_deg']);
const FRAME_V2_KEYS = new Set([...FRAME_KEYS, 'lineage', 'coverage', 'transport_dropped',
  'channel', 'frequency_hz', 'sample_rate_hz', 'lane_first_input_sample',
  'lane_end_input_sample', 'tracker_diagnostics']);
const CANDIDATE_V2_KEYS = new Set([...CANDIDATE_KEYS, 'kind', 'reason', 'state', 'previous_state',
  'candidate_id', 'predicted_delay_km', 'predicted_doppler_hz', 'delay_residual_km',
  'doppler_residual_hz', 'delay_gate_km', 'doppler_gate_hz', 'measured',
  'confirmation_credit', 'support_depth', 'gate_rejection_mask', 'residual_search_truncated',
  'spawned_first_track_id', 'spawned_track_count']);
const LINEAGE_KEYS = new Set(['epoch', 'generation', 'consumed_sequence', 'clock_precision_ns',
  'input_epoch', 'first_sample', 'end_sample']);
const COVERAGE_KEYS = new Set(['candidates_total', 'tracks_total', 'candidates_retained',
  'tracks_retained', 'truncated']);
const NATIVE_REASONS = new Set(['associated', 'minimum_snr', 'doppler_comb', 'range_comb',
  'persistent_occupancy', 'boundary', 'capacity', 'seeded', 'miss', 'tentative_expired',
  'inactivity_deleted', 'confirmed', 'retained', 'reset_input', 'duplicate']);
const PRODUCER_KEYS = new Set(['build_id', 'config_fingerprint']);
const TRACKER_DIAGNOSTIC_KEYS = new Set(['no_op', 'detections', 'tracks_before',
  'tracks_after', 'candidate_track_pairs', 'pair_already_claimed',
  'pair_delay_rejected', 'pair_doppler_rejected', 'pair_innovation_rejected',
  'pair_in_gate', 'associations', 'misses', 'promotions', 'tentative_expired',
  'inactivity_deleted', 'birth_candidates', 'birth_snr_rejected',
  'birth_ridge_rejected', 'initiated',
]);
const STAGES = new Set(['input', 'candidate_generation', 'candidate_pruning', 'association', 'branch_survival', 'track_publication', 'tracker_frame']);
const REASONS = new Set(['interval', 'no_candidates', 'candidate_admitted', 'candidate_rejected',
  'branch_continues', 'branch_terminated', 'not_emitted', 'reset_input', 'duplicate']);

function rejectTruthFields(value, name = 'frame') {
  if (Array.isArray(value)) return value.forEach((item, index) => rejectTruthFields(item, `${name}[${index}]`));
  if (!object(value)) return;
  for (const [key, child] of Object.entries(value)) {
    if (truthKey.test(key)) throw new Error(`Truth-shaped field is forbidden: ${name}.${key}`);
    rejectTruthFields(child, `${name}.${key}`);
  }
}

function onlyKeys(value, allowed, name) {
  for (const key of Object.keys(value))
    if (!allowed.has(key)) throw new Error(`Unknown causal funnel field: ${name}.${key}`);
}

function validateFrame(frame) {
  rejectTruthFields(frame);
  if (!object(frame)) throw new Error('Invalid causal funnel frame');
  const version2 = frame.schema === 'vectorwarp-causal-funnel-frame-v2';
  onlyKeys(frame, version2 ? FRAME_V2_KEYS : FRAME_KEYS, 'frame');
  const exactTime = version2 ? exactUint64(frame.event_time_ns) && exactUint64(frame.sequence) :
    Number.isSafeInteger(frame.event_time_ns) && frame.event_time_ns >= 0 &&
    Number.isSafeInteger(frame.sequence) && frame.sequence >= 0;
  if ((!version2 && frame.schema !== 'vectorwarp-causal-funnel-frame-v1') ||
      (!version2 && (frame.stage === 'tracker_frame' || ['reset_input','duplicate'].includes(frame.reason))) ||
      !['synthetic', 'native'].includes(frame.source) || !exactTime ||
      !STAGES.has(frame.stage) || !REASONS.has(frame.reason) ||
      !Array.isArray(frame.candidates) || frame.candidates.length > 64 ||
      (frame.candidates.length === 0 && !['no_candidates', 'reset_input', 'duplicate'].includes(frame.reason)) ||
      (frame.candidates.length !== 0 && ['no_candidates','duplicate','reset_input'].includes(frame.reason)))
    throw new Error('Invalid causal funnel frame');
  if (version2 && frame.source === 'native') {
    if (frame.transport_dropped !== undefined && !exactUint64(frame.transport_dropped))
      throw new Error('Invalid exact transport drop count');
    if (!object(frame.lineage)) throw new Error('Native causal lineage required');
    onlyKeys(frame.lineage, LINEAGE_KEYS, 'lineage');
    for (const key of ['epoch', 'generation', 'consumed_sequence', 'clock_precision_ns'])
      if (!exactUint64(frame.lineage[key])) throw new Error('Invalid exact causal lineage');
    if (frame.lineage.clock_precision_ns === '0') throw new Error('Invalid clock precision');
    for (const key of ['input_epoch', 'first_sample', 'end_sample'])
      if (frame.lineage[key] !== undefined && !exactUint64(frame.lineage[key]))
        throw new Error('Invalid capture sample lineage');
    // Raw capture bounds are half-open; lane bounds below keep their native
    // inclusive last-sample convention and may describe a single sample.
    if ((frame.lineage.input_epoch === undefined) !== (frame.lineage.first_sample === undefined) ||
        (frame.lineage.first_sample === undefined) !== (frame.lineage.end_sample === undefined) ||
        (frame.lineage.first_sample !== undefined &&
          BigInt(frame.lineage.end_sample) <= BigInt(frame.lineage.first_sample)))
      throw new Error('Invalid capture sample interval');
    if (frame.channel !== undefined &&
        (typeof frame.channel !== 'string' ||
         !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(frame.channel)))
      throw new Error('Invalid causal channel');
    for (const key of ['frequency_hz', 'sample_rate_hz'])
      if (frame[key] !== undefined &&
          (!Number.isSafeInteger(frame[key]) || frame[key] < 1))
        throw new Error('Invalid causal RF identity');
    for (const key of ['lane_first_input_sample', 'lane_end_input_sample'])
      if (frame[key] !== undefined && !exactUint64(frame[key]))
        throw new Error('Invalid causal lane lineage');
    if ((frame.lane_first_input_sample === undefined) !==
        (frame.lane_end_input_sample === undefined) ||
        (frame.lane_first_input_sample !== undefined &&
         BigInt(frame.lane_end_input_sample) < BigInt(frame.lane_first_input_sample)))
      throw new Error('Invalid causal lane interval');
    if (frame.tracker_diagnostics !== undefined) {
      if (!object(frame.tracker_diagnostics))
        throw new Error('Invalid causal tracker diagnostics');
      onlyKeys(frame.tracker_diagnostics, TRACKER_DIAGNOSTIC_KEYS,
        'frame.tracker_diagnostics');
      for (const [key, value] of Object.entries(frame.tracker_diagnostics))
        if (key === 'no_op' ? typeof value !== 'boolean' :
          !Number.isSafeInteger(value) || value < 0)
          throw new Error('Invalid causal tracker diagnostic');
    }
  }
  if (frame.coverage !== undefined) {
    if (!version2 || !object(frame.coverage)) throw new Error('Invalid causal coverage');
    onlyKeys(frame.coverage, COVERAGE_KEYS, 'coverage');
    for (const key of [...COVERAGE_KEYS].filter(key => key !== 'truncated'))
      if (!Number.isSafeInteger(frame.coverage[key]) || frame.coverage[key] < 0)
        throw new Error('Invalid causal coverage count');
    const c = frame.coverage;
    if (c.candidates_retained > c.candidates_total || c.tracks_retained > c.tracks_total ||
        c.candidates_retained + c.tracks_retained !== frame.candidates.length ||
        c.truncated !== (c.candidates_retained < c.candidates_total || c.tracks_retained < c.tracks_total))
      throw new Error('Inconsistent causal coverage');
  }
  if (frame.producer !== undefined) {
    if (!object(frame.producer)) throw new Error('Invalid causal funnel producer identity');
    onlyKeys(frame.producer, PRODUCER_KEYS, 'frame.producer');
    for (const name of PRODUCER_KEYS) if (frame.producer[name] !== undefined &&
        (typeof frame.producer[name] !== 'string' || frame.producer[name].length < 1 ||
         frame.producer[name].length > 128)) throw new Error('Invalid causal funnel producer identity');
  }
  const ids = new Set();
  for (const candidate of frame.candidates) {
    if (!object(candidate)) throw new Error('Invalid causal funnel candidate');
    onlyKeys(candidate, version2 ? CANDIDATE_V2_KEYS : CANDIDATE_KEYS, 'frame.candidates');
    if (typeof candidate.id !== 'string' ||
        !/^[A-Za-z0-9._-]{1,64}$/.test(candidate.id) || ids.has(candidate.id) ||
        !finite(candidate.delay_km) || !finite(candidate.doppler_hz) ||
        !(finite(candidate.snr_db) || (version2 && candidate.snr_db === null)) || !Array.isArray(candidate.bearing_candidates_deg) ||
        candidate.bearing_candidates_deg.length < (version2 ? 0 : 1) || candidate.bearing_candidates_deg.length > 8 ||
        candidate.bearing_candidates_deg.some(value => !finite(value) || value < 0 || value >= 360))
      throw new Error('Invalid causal funnel candidate');
    if (version2) {
      if (!['candidate', 'track'].includes(candidate.kind) || !NATIVE_REASONS.has(candidate.reason) ||
          !Number.isInteger(candidate.state) || candidate.state < 0 || candidate.state > 3 ||
          !Number.isInteger(candidate.previous_state) || candidate.previous_state < 0 || candidate.previous_state > 3 ||
          typeof candidate.measured !== 'boolean' ||
          !(typeof candidate.confirmation_credit === 'boolean' ||
            (candidate.kind === 'candidate' && candidate.confirmation_credit === null)) ||
          ![0, 1, 2, 4, 8].includes(candidate.support_depth))
        throw new Error('Invalid causal transition metadata');
      if (candidate.spawned_first_track_id !== undefined || candidate.spawned_track_count !== undefined) {
        const id = candidate.spawned_first_track_id;
        if (candidate.kind !== 'candidate' || candidate.reason !== 'seeded' ||
            candidate.confirmation_credit !== true || typeof id !== 'string' ||
            !/^t(0|[1-9][0-9]{0,4})$/.test(id) || Number(id.slice(1)) >= 65535 ||
            !Number.isInteger(candidate.spawned_track_count) || candidate.spawned_track_count < 1 ||
            candidate.spawned_track_count > 131071)
          throw new Error('Invalid causal seed linkage');
      }
      for (const key of ['predicted_delay_km', 'predicted_doppler_hz', 'delay_residual_km',
        'doppler_residual_hz', 'delay_gate_km', 'doppler_gate_hz'])
        if (candidate[key] !== undefined && candidate[key] !== null && !finite(candidate[key]))
          throw new Error('Invalid causal gate residual');
      if (candidate.candidate_id !== undefined && candidate.candidate_id !== null &&
          (typeof candidate.candidate_id !== 'string' || !/^[A-Za-z0-9._-]{1,64}$/.test(candidate.candidate_id)))
        throw new Error('Invalid causal candidate link');
      if (candidate.gate_rejection_mask !== undefined &&
          (!Number.isInteger(candidate.gate_rejection_mask) || candidate.gate_rejection_mask < 0 || candidate.gate_rejection_mask > 63))
        throw new Error('Invalid causal gate rejection mask');
      if (candidate.residual_search_truncated !== undefined && typeof candidate.residual_search_truncated !== 'boolean')
        throw new Error('Invalid causal residual coverage');
    }
    ids.add(candidate.id);
  }
  return frame;
}

function canonicalEpoch(config) {
  const options = config.process?.evidence?.causal_funnel || {};
  return {schema: 'vectorwarp-causal-funnel-epoch-v2', epoch_id: options.epoch_id,
    frame_schemas: [1, 2], event_time_v2: 'uint64-decimal-nanoseconds', truth_blind: true,
    config: {max_candidates: options.max_candidates, max_queue: options.max_queue, max_queue_mb: options.max_queue_mb,
      max_file_mb: options.max_file_mb, max_files: options.max_files,
      api_ingest_enabled: options.api_ingest_enabled === true}};
}

function encodeEpoch(epoch) {
  return JSON.stringify({...epoch, fingerprint: crypto.createHash('sha256')
    .update(JSON.stringify(epoch)).digest('hex')}) + '\n';
}

class CausalFunnelRecorder {
  constructor(config, {fileSystem = fs, now = () => Date.now()} = {}) {
    this.options = {...(config.process?.evidence?.causal_funnel || {})};
    this.fs = fileSystem;
    this.now = now;
    this.queue = []; this.queueBytes = 0;
    this.writing = false;
    this.segment = 0;
    this.epochPrepared = false;
    this.epoch = canonicalEpoch(config);
    // Native socket tracing and API-ingested multi-illuminator tracing are
    // separate capabilities. The latter must not make a non-resident native
    // processor attempt the resident-only diagnostic path.
    this.nativeEnabled = this.options.enabled === true;
    this.apiIngestEnabled = this.options.api_ingest_enabled === true;
    this.enabled = this.nativeEnabled || this.apiIngestEnabled;
    this.state = {schema: 'vectorwarp-causal-funnel-status-v1', enabled: this.enabled,
      native_enabled: this.nativeEnabled, api_ingest_enabled: this.apiIngestEnabled,
      active: false, epoch_id: this.options.epoch_id || null, truth_blind: true,
      synthetic_only: false, queued: 0, queued_bytes: 0, accepted: 0, written: 0, dropped: 0,
      rejected: 0, native_received: 0, rotations: 0, retained_files: 0, last_write_at: null, error: null};
  }

  status() { return {...this.state, queued: this.queue.length, queued_bytes: this.queueBytes, active: this.writing}; }

  accept(text, {nativeTransport = false} = {}) {
    if (!this.enabled) return false;
    let frame, record, bytes;
    try {
      const inputBytes = Buffer.byteLength(typeof text === 'string' ? text : JSON.stringify(text));
      if (inputBytes > this.options.max_file_mb * 1024 * 1024) throw new Error('Causal funnel frame exceeds byte bound');
      frame = validateFrame(typeof text === 'string' ? JSON.parse(text) : text);
      record = {schema: 'vectorwarp-causal-funnel-record-v1', epoch_id: this.options.epoch_id,
        recorded_at_ms: this.now(), frame};
      bytes = Buffer.byteLength(`${JSON.stringify(record)}\n`);
      if (bytes > this.options.max_file_mb * 1024 * 1024 || bytes > this.options.max_queue_mb * 1024 * 1024)
        throw new Error('Causal funnel record exceeds byte bound');
    }
    catch (error) { this.state.rejected += 1; this.state.error = error.message; return false; }
    if (frame.candidates.length > this.options.max_candidates) {
      this.state.rejected += 1; this.state.error = 'Causal funnel frame exceeds configured candidate bound'; return false;
    }
    if (this.queue.length >= this.options.max_queue ||
        this.queueBytes + bytes > this.options.max_queue_mb * 1024 * 1024) {
      this.state.dropped += 1; return false;
    }
    this.queue.push({record, bytes}); this.queueBytes += bytes; this.state.accepted += 1;
    if (nativeTransport) this.state.native_received += 1;
    if (!this.writing) void this.drain();
    return true;
  }

  async flush() { if (!this.writing && this.queue.length) void this.drain(); while (this.writing) await new Promise(resolve => setTimeout(resolve, 1)); }
  manifestFile() { return path.join(this.options.path, `${this.options.epoch_id}.manifest.json`); }
  segmentFile() { return path.join(this.options.path, `${this.options.epoch_id}-${String(this.segment).padStart(6, '0')}.jsonl`); }

  async prepareEpoch() {
    await this.fs.promises.mkdir(this.options.path, {recursive: true, mode: 0o700});
    const manifest = encodeEpoch(this.epoch);
    try { await this.fs.promises.writeFile(this.manifestFile(), manifest, {flag: 'wx', mode: 0o600}); }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      const existing = await this.fs.promises.readFile(this.manifestFile(), 'utf8');
      // Older v2 native-only epochs omitted this subsequently added false
      // default. Accept exactly that prior signed representation, preserving
      // its bytes. Enabled API ingestion or any other change remains a new
      // contract and must not reuse the immutable epoch.
      if (existing !== manifest) {
        const nativeOnly = {...this.epoch, config: {...this.epoch.config}};
        delete nativeOnly.config.api_ingest_enabled;
        if (this.epoch.config.api_ingest_enabled !== false || existing !== encodeEpoch(nativeOnly))
          throw new Error('Causal funnel epoch manifest is immutable and does not match this configuration');
      }
    }
    await this.fs.promises.chmod(this.manifestFile(), 0o600);
    if (!this.epochPrepared) {
      const prefix = `${this.options.epoch_id}-`;
      const segments = (await this.fs.promises.readdir(this.options.path)).map(name => {
        const suffix = name.startsWith(prefix) && name.endsWith('.jsonl') ?
          name.slice(prefix.length, -'.jsonl'.length) : '';
        return /^\d{6}$/.test(suffix) ? Number(suffix) : null;
      }).filter(Number.isInteger);
      if (segments.length) this.segment = Math.max(...segments);
      this.epochPrepared = true;
    }
  }

  async rotateIfNeeded(bytes) {
    const limit = this.options.max_file_mb * 1024 * 1024;
    let size = 0;
    try { size = (await this.fs.promises.stat(this.segmentFile())).size; }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    if (size && size + bytes > limit) { this.segment += 1; this.state.rotations += 1; }
    const prefix = `${this.options.epoch_id}-`;
    const files = (await this.fs.promises.readdir(this.options.path)).filter(name =>
      name.startsWith(prefix) && name.endsWith('.jsonl')).sort();
    while (files.length >= this.options.max_files && !files.includes(path.basename(this.segmentFile()))) {
      await this.fs.promises.unlink(path.join(this.options.path, files.shift()));
    }
    this.state.retained_files = files.length;
  }

  async write(item) {
    await this.prepareEpoch();
    const line = `${JSON.stringify(item.record)}\n`;
    if (item.bytes !== Buffer.byteLength(line) || item.bytes > this.options.max_file_mb * 1024 * 1024)
      throw new Error('Causal funnel record exceeds byte bound');
    await this.rotateIfNeeded(item.bytes);
    await this.fs.promises.appendFile(this.segmentFile(), line, {encoding: 'utf8', mode: 0o600});
    this.state.written += 1; this.state.last_write_at = this.now();
    // The record is durable once append succeeds. A later permission or
    // retention-status read failure is an operational error, not record loss.
    try {
      await this.fs.promises.chmod(this.segmentFile(), 0o600);
      this.state.retained_files = (await this.fs.promises.readdir(this.options.path)).filter(name =>
        name.startsWith(`${this.options.epoch_id}-`) && name.endsWith('.jsonl')).length;
    } catch (error) { this.state.error = error.message; }
  }

  async drain() {
    this.writing = true;
    try {
      while (this.queue.length) {
        const item = this.queue.shift(); this.queueBytes -= item.bytes;
        await this.write(item);
      }
    } catch (error) {
      this.state.error = error.message;
      this.state.dropped += 1 + this.queue.length;
      this.queue.length = 0; this.queueBytes = 0;
    }
    finally { this.writing = false; }
  }
}

function createCausalFunnelRecorder(config, options) { return new CausalFunnelRecorder(config, options); }
module.exports = {CausalFunnelRecorder, createCausalFunnelRecorder, validateFrame, rejectTruthFields};
