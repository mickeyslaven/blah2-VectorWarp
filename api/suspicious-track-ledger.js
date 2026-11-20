'use strict';

// Truth-blind, bounded retention of persistent moving tracks and their
// cross-illuminator coincidences. Truth assessment is deliberately a separate
// post-inference operation and cannot affect candidate creation or ranking.

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const C_KM_S = 299792.458;
const OPERATIONAL_STATES = new Set(['ACTIVE', 'CONFIRMED', 'COASTING']);
const finite = value => typeof value === 'number' && Number.isFinite(value);
const clone = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));
const median = values => {
  const sorted = values.filter(finite).sort((a, b) => a - b);
  if (!sorted.length) return null;
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};
const locationKey = value => value && [value.latitude, value.longitude, value.altitude]
  .every(item => finite(Number(item))) ? [value.latitude, value.longitude, value.altitude]
    .map(item => Number(item).toFixed(6)).join(':') : null;
const eventId = keys => `suspicious-${crypto.createHash('sha256')
  .update(keys.slice().sort().join('|')).digest('hex').slice(0, 20)}`;

const DEFAULTS = Object.freeze({gapMs: 3000, coincidenceSlopMs: 3000,
  minimumDurationMs: 5000, minimumMeasuredSamples: 6, minimumMeasuredFraction: .5,
  minimumAbsoluteDopplerHz: 10, minimumDelayMotionKm: .03,
  minimumDopplerMotionHz: 3, sameSiteDelayToleranceKm: .35,
  sameSiteNormalizedDopplerToleranceHz: 8, maximumSamplesPerTrack: 4096,
  maximumActiveTracks: 512, maximumRetainedTracks: 512,
  maximumEvents: 256, maximumTotalSamples: 100000, maximumCandidateTracks: 128,
  maximumFileBytes: 32 * 1024 * 1024, coincidenceIntervalMs: 1000,
  persistIntervalMs: 30000});

function publicTrack(track, includeSamples = true) {
  const measured = track.samples.filter(sample => sample.measured !== false);
  return {track_key: track.key, channel: track.channel, track_id: track.id,
    frequency_hz: track.frequency_hz, transmitter_site: track.transmitter_site,
    started_at: track.started_at, last_at: track.last_at,
    duration_ms: Math.max(0, track.last_at - track.started_at),
    observations: track.samples.length, measured_observations: measured.length,
    measured_fraction: track.samples.length ? measured.length / track.samples.length : null,
    delay_motion_km: track.delay_motion_km, doppler_motion_hz: track.doppler_motion_hz,
    median_delay_km: median(measured.map(sample => sample.delay_km)),
    median_doppler_hz: median(measured.map(sample => sample.doppler_hz)),
    ...(includeSamples ? {samples: clone(track.samples)} : {})};
}

class SuspiciousTrackLedger {
  constructor(definitions = [], options = {}) {
    this.options = {...DEFAULTS, ...options};
    this.filename = options.filename || null;
    this.now = options.now || Date.now;
    this.definitions = new Map(definitions.map(item => [String(item.id), clone(item)]));
    this.active = new Map();
    this.retained = [];
    this.events = [];
    this.assessments = new Map();
    this.counters = {accepted_frames: 0, accepted_samples: 0, invalid_frames: 0,
      invalid_samples: 0, truncated_track_samples: 0, evicted_active_tracks: 0,
      evicted_retained_tracks: 0, evicted_events: 0, persistence_errors: 0};
    this.lastPersistAt = 0;
    this.lastCoincidenceAt = 0;
    this.closed = false;
    this.load();
  }

  load() {
    if (!this.filename) return;
    try {
      const value = JSON.parse(fs.readFileSync(this.filename, 'utf8'));
      if (value?.schema !== 'vectorwarp-suspicious-track-ledger-v1') return;
      this.retained = Array.isArray(value.retained_tracks) ? value.retained_tracks : [];
      this.events = Array.isArray(value.events) ? value.events : [];
      this.assessments = new Map((value.assessments || []).map(item => [item.event_id, item]));
      Object.assign(this.counters, value.counters || {});
    } catch (error) {
      if (error.code !== 'ENOENT') this.counters.persistence_errors += 1;
    }
  }

  trackKey(channel, id, startedAt) { return `${channel}:${id}:${startedAt}`; }

  eligible(track) {
    const measured = track.samples.filter(sample => sample.measured !== false);
    const duration = track.last_at - track.started_at;
    const fraction = measured.length / Math.max(1, track.samples.length);
    const doppler = median(measured.map(sample => Math.abs(sample.doppler_hz))) || 0;
    return duration >= this.options.minimumDurationMs &&
      measured.length >= this.options.minimumMeasuredSamples &&
      fraction >= this.options.minimumMeasuredFraction &&
      doppler >= this.options.minimumAbsoluteDopplerHz &&
      (track.delay_motion_km >= this.options.minimumDelayMotionKm ||
       track.doppler_motion_hz >= this.options.minimumDopplerMotionHz);
  }

  accept(frame) {
    const channel = String(frame?.id || '');
    const definition = this.definitions.get(channel);
    const tracks = frame?.tracks;
    if (!definition || !finite(frame?.timestamp) || !Array.isArray(tracks?.data)) {
      this.counters.invalid_frames += 1; return false;
    }
    this.counters.accepted_frames += 1;
    const seen = new Set();
    const kmPerBin = C_KM_S / Number(definition.output_sample_rate_hz);
    for (const item of tracks.data) {
      if (!item || !OPERATIONAL_STATES.has(String(item.state).toUpperCase()) ||
          item.id === null || item.id === undefined || !finite(item.delay) ||
          !finite(item.doppler) || !finite(kmPerBin)) {
        this.counters.invalid_samples += 1; continue;
      }
      const id = String(item.id), activeKey = `${channel}:${id}`;
      seen.add(activeKey);
      let track = this.active.get(activeKey);
      if (!track || frame.timestamp - track.last_at > this.options.gapMs ||
          (Number.isInteger(item.n) && Number.isInteger(track.last_n) && item.n < track.last_n)) {
        if (track) this.finalize(activeKey);
        track = {key: this.trackKey(channel, id, frame.timestamp), channel, id,
          frequency_hz: Number(definition.frequency_hz),
          transmitter_site: locationKey(definition.transmitter),
          started_at: frame.timestamp, last_at: frame.timestamp, last_n: null,
          delay_motion_km: 0, doppler_motion_hz: 0, samples: []};
        this.active.set(activeKey, track);
      }
      if (track.samples.at(-1)?.timestamp === frame.timestamp) continue;
      const delayKm = item.delay * kmPerBin;
      const prior = [...track.samples].reverse().find(sample => sample.measured !== false);
      if (item.measured !== false && prior) {
        track.delay_motion_km += Math.abs(delayKm - prior.delay_km);
        track.doppler_motion_hz += Math.abs(item.doppler - prior.doppler_hz);
      }
      track.last_at = frame.timestamp;
      track.last_n = Number.isInteger(item.n) ? item.n : track.last_n;
      const sample = {timestamp: frame.timestamp, state: String(item.state).toUpperCase(),
        measured: item.measured !== false, delay_km: delayKm, doppler_hz: item.doppler,
        association_count: Number.isInteger(item.n) ? item.n : null,
        confidence: finite(item.confidence) ? item.confidence : null,
        snr_db: finite(item.snr_db) ? item.snr_db : null,
        capture_epoch: Number.isSafeInteger(frame.capture_epoch) ? frame.capture_epoch : null,
        capture_first_sample: Number.isSafeInteger(frame.capture_first_input_sample) ?
          frame.capture_first_input_sample : null,
        capture_end_sample: Number.isSafeInteger(frame.capture_end_input_sample) ?
          frame.capture_end_input_sample : null};
      track.samples.push(sample); this.counters.accepted_samples += 1;
      if (track.samples.length > this.options.maximumSamplesPerTrack) {
        track.samples.shift(); this.counters.truncated_track_samples += 1;
      }
    }
    for (const [key, track] of this.active)
      if (!seen.has(key) && frame.timestamp - track.last_at > this.options.gapMs)
        this.finalize(key);
    this.enforceBounds();
    if (frame.timestamp - this.lastCoincidenceAt >= this.options.coincidenceIntervalMs) {
      this.findCoincidences(); this.lastCoincidenceAt = frame.timestamp;
    }
    this.maybePersist();
    return true;
  }

  finalize(key) {
    const track = this.active.get(key);
    if (!track) return;
    this.active.delete(key);
    if (this.eligible(track)) this.retained.push(publicTrack(track));
  }

  sameSiteEvidence(left, right) {
    if (!left.transmitter_site || left.transmitter_site !== right.transmitter_site) return null;
    const delayResidualKm = Math.abs(left.median_delay_km - right.median_delay_km);
    const normalizedLeft = left.median_doppler_hz / left.frequency_hz;
    const normalizedRight = right.median_doppler_hz / right.frequency_hz;
    const referenceFrequency = Math.min(left.frequency_hz, right.frequency_hz);
    const dopplerResidualHz = Math.abs(normalizedLeft - normalizedRight) * referenceFrequency;
    return {delay_residual_km: delayResidualKm,
      frequency_normalized_doppler_residual_hz: dopplerResidualHz,
      consistent: delayResidualKm <= this.options.sameSiteDelayToleranceKm &&
        dopplerResidualHz <= this.options.sameSiteNormalizedDopplerToleranceHz};
  }

  findCoincidences() {
    const tracks = [...this.retained, ...[...this.active.values()]
      // Coincidence formation needs summary statistics, not another deep copy
      // of every retained IQ-derived sample on every one-second pass.
      .filter(track => this.eligible(track)).map(track => publicTrack(track, false))]
      .sort((left, right) => right.last_at - left.last_at)
      .slice(0, this.options.maximumCandidateTracks);
    // The former implementation scanned every event and every event track key
    // for every candidate pair.  At the documented bounds that was millions of
    // string comparisons per second and could starve the API event loop.  Build
    // the inverse index once, then preserve the same connected-component merge
    // semantics with constant-time lookups.
    const eventByTrackKey = new Map();
    for (const event of this.events)
      for (const key of event.track_keys || []) eventByTrackKey.set(key, event);
    const mergeEvents = (event, other) => {
      if (!other || event === other) return event;
      event.track_keys.push(...other.track_keys);
      event.channels.push(...other.channels);
      event.distinct_transmitter_sites.push(...other.distinct_transmitter_sites);
      event.same_site_consistency.push(...other.same_site_consistency);
      for (const key of other.track_keys) eventByTrackKey.set(key, event);
      const index = this.events.indexOf(other);
      if (index >= 0) this.events.splice(index, 1);
      return event;
    };
    for (let leftIndex = 0; leftIndex < tracks.length; leftIndex += 1)
      for (let rightIndex = leftIndex + 1; rightIndex < tracks.length; rightIndex += 1) {
        const left = tracks[leftIndex], right = tracks[rightIndex];
        if (left.channel === right.channel ||
            left.last_at + this.options.coincidenceSlopMs < right.started_at ||
            right.last_at + this.options.coincidenceSlopMs < left.started_at) continue;
        const sameSite = this.sameSiteEvidence(left, right);
        if (sameSite && !sameSite.consistent) continue;
        const leftEvent = eventByTrackKey.get(left.track_key);
        const rightEvent = eventByTrackKey.get(right.track_key);
        let event = leftEvent || rightEvent;
        if (!event) {
          event = {event_id: eventId([left.track_key, right.track_key]),
            created_at: this.now(), updated_at: this.now(),
            status: 'radar-only-candidate', truth_used_for_selection: false,
            track_keys: [], channels: [], distinct_transmitter_sites: [],
            same_site_consistency: []};
          this.events.push(event);
        } else mergeEvents(event, leftEvent === event ? rightEvent : leftEvent);
        event.updated_at = this.now();
        event.track_keys = [...new Set([...event.track_keys, left.track_key, right.track_key])];
        event.channels = [...new Set([...event.channels, left.channel, right.channel])];
        event.distinct_transmitter_sites = [...new Set([...event.distinct_transmitter_sites,
          left.transmitter_site, right.transmitter_site].filter(Boolean))];
        eventByTrackKey.set(left.track_key, event);
        eventByTrackKey.set(right.track_key, event);
        if (sameSite && !event.same_site_consistency.some(item =>
            item.channels.includes(left.channel) && item.channels.includes(right.channel)))
          event.same_site_consistency.push({channels: [left.channel, right.channel], ...sameSite});
        event.evidence_class = event.distinct_transmitter_sites.length >= 2 ?
          'multiple-distinct-bistatic-geometries' : 'co-sited-frequency-corroboration';
      }
    this.enforceBounds();
  }

  attachTruthAssessment(eventIdValue, assessment) {
    if (!this.events.some(event => event.event_id === eventIdValue)) return false;
    const row = {event_id: eventIdValue, assessed_at: this.now(),
      role: 'post-inference-evaluation-only', assessment: clone(assessment)};
    this.assessments.set(eventIdValue, row); this.maybePersist(true); return true;
  }

  enforceBounds() {
    while (this.active.size > this.options.maximumActiveTracks) {
      const oldest = [...this.active.entries()].sort((a, b) => a[1].last_at - b[1].last_at)[0];
      this.active.delete(oldest[0]); this.counters.evicted_active_tracks += 1;
    }
    while (this.retained.length > this.options.maximumRetainedTracks) {
      this.retained.shift(); this.counters.evicted_retained_tracks += 1;
    }
    const sampleCount = () => this.retained.reduce((sum, track) => sum + track.samples.length, 0) +
      [...this.active.values()].reduce((sum, track) => sum + track.samples.length, 0);
    while (this.retained.length && sampleCount() > this.options.maximumTotalSamples) {
      this.retained.shift(); this.counters.evicted_retained_tracks += 1;
    }
    while (this.active.size && sampleCount() > this.options.maximumTotalSamples) {
      const oldest = [...this.active.entries()].sort((a, b) => a[1].last_at - b[1].last_at)[0];
      this.active.delete(oldest[0]); this.counters.evicted_active_tracks += 1;
    }
    while (this.events.length > this.options.maximumEvents) {
      const removed = this.events.shift(); this.assessments.delete(removed.event_id);
      this.counters.evicted_events += 1;
    }
  }

  snapshot({includeSamples = false} = {}) {
    const tracks = [...this.retained, ...[...this.active.values()]
      .filter(track => this.eligible(track)).map(track => publicTrack(track))];
    const compact = track => includeSamples ? clone(track) : {...clone(track), samples: undefined};
    return {schema: 'vectorwarp-suspicious-track-ledger-v1',
      selection_policy: 'truth-blind-radar-kinematics-and-cross-illuminator-consistency',
      decision_effect: 'observation-only', bounds: clone(this.options),
      counters: clone(this.counters), active_track_lifecycles: this.active.size,
      retained_tracks: tracks.map(compact), events: clone(this.events),
      assessments: clone([...this.assessments.values()])};
  }

  maybePersist(force = false) {
    if (!this.filename || this.closed || !force && this.now() - this.lastPersistAt <
        this.options.persistIntervalMs) return;
    try {
      fs.mkdirSync(path.dirname(this.filename), {recursive: true});
      const temporary = `${this.filename}.${process.pid}.tmp`;
      let serialized = `${JSON.stringify(this.snapshot({includeSamples: true}))}\n`;
      while (Buffer.byteLength(serialized) > this.options.maximumFileBytes &&
          (this.retained.length || this.active.size)) {
        if (this.retained.length) {
          this.retained.shift(); this.counters.evicted_retained_tracks += 1;
        } else {
          const oldest = [...this.active.entries()]
            .sort((a, b) => a[1].last_at - b[1].last_at)[0];
          this.active.delete(oldest[0]); this.counters.evicted_active_tracks += 1;
        }
        serialized = `${JSON.stringify(this.snapshot({includeSamples: true}))}\n`;
      }
      fs.writeFileSync(temporary, serialized,
        {encoding: 'utf8', mode: 0o600});
      fs.renameSync(temporary, this.filename);
      this.lastPersistAt = this.now();
    } catch (_) { this.counters.persistence_errors += 1; }
  }

  close() {
    if (this.closed) return;
    for (const key of [...this.active.keys()]) this.finalize(key);
    this.findCoincidences(); this.maybePersist(true); this.closed = true;
  }
}

module.exports = {SuspiciousTrackLedger, DEFAULTS};
