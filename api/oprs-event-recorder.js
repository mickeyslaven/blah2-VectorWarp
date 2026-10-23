'use strict';

// Truth-blind, append-only capture of the receiver outputs needed by OPRS v2.
// This recorder deliberately has no ADS-B/truth input and performs no scoring.

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const digest = value => crypto.createHash('sha256').update(value).digest('hex');

class OprsEventRecorder {
  constructor(config, options = {}) {
    this.filename = options.filename || null;
    this.enabled = Boolean(this.filename);
    this.now = options.now || Date.now;
    this.runId = options.runId || crypto.randomUUID();
    this.sequence = 0;
    this.closed = false;
    this.tracks = new Map();
    const sampleRate = Number(config?.capture?.fs);
    const cpiSeconds = Number(config?.process?.data?.cpi);
    this.kmPerBin = finite(sampleRate) && sampleRate > 0 ? 299792.458 / sampleRate : null;
    this.cpiDurationMs = finite(cpiSeconds) && cpiSeconds > 0 ? cpiSeconds * 1000 : null;
    if (!this.enabled) return;
    if (!this.kmPerBin) throw new Error('OPRS recording requires capture.fs');
    fs.mkdirSync(path.dirname(this.filename), {recursive: true});
    this.append({kind: 'run_start', schema: 'oprs-event-log-v1', run_id: this.runId,
      recorded_at_ms: this.now(), delay_units: {detection: 'km', track_input: 'bin', output: 'km'},
      sample_rate_hz: sampleRate, cpi_duration_ms: this.cpiDurationMs});
  }

  append(row) {
    if (!this.enabled || this.closed) return;
    fs.appendFileSync(this.filename, `${JSON.stringify({...row, sequence: this.sequence++})}\n`,
      {encoding: 'utf8', mode: 0o600});
  }

  exactEventTime(frame, stream) {
    const value = frame?.eventTime;
    // Backward compatibility for inactive receivers: only an explicit
    // provenance claim is adjudicated here. Kraken now always supplies one.
    if (value === undefined) return true;
    if (value && value.exact === true && value.uncertaintyBounded === true &&
        value.uncertaintyNs === 0) return true;
    const source = typeof value?.source === 'string' ? value.source.slice(0, 128) : 'invalid';
    const reason = typeof value?.reason === 'string' ? value.reason.slice(0, 240) :
      'Event-time provenance is missing or not exact';
    this.append({kind: 'event_time_rejection', run_id: this.runId, stream,
      reported_time_ms: finite(frame?.timestamp) ? Math.trunc(frame.timestamp) : null,
      source, reason});
    return false;
  }

  acceptDetection(frame) {
    if (!this.enabled || !finite(frame?.timestamp) || !Array.isArray(frame.delay) ||
        !Array.isArray(frame.doppler)) return;
    if (!this.exactEventTime(frame, 'detection')) return;
    const timestamp = Math.trunc(frame.timestamp);
    const count = Math.min(frame.delay.length, frame.doppler.length);
    this.append({kind: 'detection_frame', run_id: this.runId, event_time_ms: timestamp,
      output_count: count});
    for (let index = 0; index < count; ++index) {
      if (!finite(frame.delay[index]) || !finite(frame.doppler[index])) continue;
      const id = digest(`${this.runId}|d|${timestamp}|${index}|${frame.delay[index]}|${frame.doppler[index]}`);
      this.append({kind: 'detection', run_id: this.runId, event_time_ms: timestamp,
        detection_id: `d-${id}`, delay_km: frame.delay[index], doppler_hz: frame.doppler[index]});
    }
  }

  acceptTracker(frame) {
    if (!this.enabled || !finite(frame?.timestamp) || !Array.isArray(frame.data)) return;
    if (!this.exactEventTime(frame, 'tracker')) return;
    const timestamp = Math.trunc(frame.timestamp);
    this.append({kind: 'tracker_frame', run_id: this.runId, event_time_ms: timestamp,
      output_count: frame.data.length});
    for (const candidate of frame.data) {
      if (!candidate || candidate.state !== 'ACTIVE' || candidate.id === undefined ||
          !finite(candidate.delay) || !finite(candidate.doppler)) continue;
      const rawId = String(candidate.id);
      const associations = Number.isInteger(candidate.n) ? candidate.n : null;
      let lifecycle = this.tracks.get(rawId);
      if (!lifecycle) lifecycle = {generation: 0, lastAssociations: associations, confirmed: false};
      else if (associations !== null && lifecycle.lastAssociations !== null &&
               associations < lifecycle.lastAssociations) {
        lifecycle = {generation: lifecycle.generation + 1,
          lastAssociations: associations, confirmed: false};
      }
      lifecycle.lastAssociations = associations;
      const trackId = `${this.runId}:${rawId}:${lifecycle.generation}`;
      const delayKm = candidate.delay * this.kmPerBin;
      const id = digest(`${trackId}|${timestamp}|${associations}|${delayKm}|${candidate.doppler}`);
      const measurementId = `m-${id}`;
      this.append({kind: 'track_measurement', run_id: this.runId, event_time_ms: timestamp,
        measurement_id: measurementId, track_id: trackId, delay_km: delayKm,
        doppler_hz: candidate.doppler});
      if (!lifecycle.confirmed) {
        this.append({kind: 'track_initiation', run_id: this.runId, event_time_ms: timestamp,
          track_id: trackId, measurement_id: measurementId});
        lifecycle.confirmed = true;
      }
      this.tracks.set(rawId, lifecycle);
    }
  }

  acceptTiming(frame) {
    if (!this.enabled || !finite(frame?.timestamp)) return;
    if (!this.exactEventTime(frame, 'timing')) return;
    const fields = {};
    for (const [name, value] of Object.entries(frame))
      if (name !== 'timestamp' && finite(value)) fields[name] = value;
    this.append({kind: 'timing_frame', run_id: this.runId,
      event_time_ms: Math.trunc(frame.timestamp), cpi_duration_ms: this.cpiDurationMs,
      fields});
  }

  close() {
    if (!this.enabled || this.closed) return;
    this.append({kind: 'run_end', schema: 'oprs-event-log-v1', run_id: this.runId,
      recorded_at_ms: this.now()});
    this.closed = true;
  }
}

function createOprsEventRecorder(config, options = {}) {
  return new OprsEventRecorder(config, options);
}

module.exports = {OprsEventRecorder, createOprsEventRecorder};
