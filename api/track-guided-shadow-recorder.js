'use strict';

const fs = require('fs');
const path = require('path');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const normalizedHex = value => typeof value === 'string' ? value.trim().toLowerCase() : '';

class TrackGuidedShadowRecorder {
  constructor(config, adsbSource, {fileSystem = fs, now = Date.now} = {}) {
    const options = config.process?.tracker?.track_guided_shadow || {};
    this.enabled = options.enable === true && config.truth?.adsb?.enabled === true;
    this.options = {
      path: options.evidence_path || path.join(config.save?.path || '/tmp/',
        'track-guided-shadow-evidence.jsonl'),
      maxFileMb: Number.isInteger(options.max_file_mb) ? options.max_file_mb : 10,
      maxFiles: Number.isInteger(options.max_files) ? options.max_files : 3,
      maxTruthAgeS: finite(options.max_truth_age_s) ? options.max_truth_age_s : 3,
      delayToleranceKm: finite(config.truth?.adsb?.delay_tolerance) ?
        config.truth.adsb.delay_tolerance : 2,
      dopplerToleranceHz: finite(config.truth?.adsb?.doppler_tolerance) ?
        config.truth.adsb.doppler_tolerance : 5
    };
    this.frequencyHz = config.capture?.fc;
    this.adsbSource = adsbSource;
    this.fs = fileSystem;
    this.now = now;
    this.queue = [];
    this.running = false;
    this.recent = [];
    this.counters = {frames: 0, recoveries: 0, matched: 0, unmatched: 0,
      ambiguous: 0, staleTruth: 0, invalid: 0, droppedFrames: 0};
    this.lastWriteAt = null;
    this.error = null;
  }

  status() {
    return {enabled: this.enabled, path: this.options.path, active: this.running,
      queued: this.queue.length, lastWriteAt: this.lastWriteAt, error: this.error,
      counters: {...this.counters}, recent: [...this.recent]};
  }

  accept(value) {
    if (!this.enabled) return;
    try {
      if (typeof value === 'string') value = JSON.parse(value);
      if (!value || !Object.prototype.hasOwnProperty.call(value, 'track_guided_shadow')) return;
      if (!finite(value.timestamp) || !Array.isArray(value.track_guided_shadow))
        throw new Error('Invalid track-guided shadow frame');
      const recoveries = value.track_guided_shadow.filter(item => item &&
        typeof item.track_id === 'string' && ['tentative', 'established'].includes(item.maturity) &&
        finite(item.delay_bin) && finite(item.delay_km) && finite(item.doppler_hz));
      if (recoveries.length !== value.track_guided_shadow.length)
        throw new Error('Invalid track-guided shadow recovery');
      if (!recoveries.length) return;
      ++this.counters.frames; this.counters.recoveries += recoveries.length;
      if (this.queue.length >= 32) { this.queue.shift(); ++this.counters.droppedFrames; }
      this.queue.push({timestamp: value.timestamp, recoveries});
      if (!this.running) void this.drain();
    } catch (error) { ++this.counters.invalid; this.error = error.message; }
  }

  async rotate(bytes) {
    const filename = this.options.path, limit = this.options.maxFileMb * 1024 * 1024;
    let size = 0;
    try { size = (await this.fs.promises.stat(filename)).size; }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    if (size + bytes <= limit) return;
    if (this.options.maxFiles === 1) {
      try { await this.fs.promises.unlink(filename); }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
      return;
    }
    for (let index = this.options.maxFiles - 1; index >= 1; --index) {
      const from = index === 1 ? filename : `${filename}.${index - 1}`;
      try { await this.fs.promises.rename(from, `${filename}.${index}`); }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
  }

  match(frameTimestamp, recovery, aircraftByHex, predictions) {
    const candidates = [];
    for (const [hex, truth] of Object.entries(predictions || {})) {
      if (!finite(truth?.timestamp) ||
          Math.abs(frameTimestamp / 1000 - truth.timestamp) > this.options.maxTruthAgeS) {
        ++this.counters.staleTruth; continue;
      }
      if (!finite(truth.delay) || !finite(truth.doppler)) continue;
      const delayError = recovery.delay_km - truth.delay;
      const dopplerError = recovery.doppler_hz - truth.doppler;
      if (Math.abs(delayError) >= this.options.delayToleranceKm ||
          Math.abs(dopplerError) >= this.options.dopplerToleranceHz) continue;
      const score = (delayError / this.options.delayToleranceKm) ** 2 +
        (dopplerError / this.options.dopplerToleranceHz) ** 2;
      candidates.push({hex: normalizedHex(hex), truth, delayError, dopplerError, score});
    }
    candidates.sort((left, right) => left.score - right.score);
    if (!candidates.length) return {classification: 'unmatched', adsb: null};
    if (candidates.length > 1 && candidates[1].score <= candidates[0].score * 1.15 + .05)
      return {classification: 'ambiguous', adsb: null};
    const best = candidates[0], aircraft = aircraftByHex.get(best.hex) || {};
    return {classification: 'matched', adsb: {hex: best.hex,
      flight: best.truth.flight || aircraft.flight || '', lat: aircraft.lat, lon: aircraft.lon,
      alt_baro: aircraft.alt_baro, alt_geom: aircraft.alt_geom, gs: aircraft.gs,
      track: aircraft.track, expected_delay_km: best.truth.delay,
      expected_doppler_hz: best.truth.doppler, position_timestamp_s: best.truth.timestamp,
      delay_residual_km: best.delayError, doppler_residual_hz: best.dopplerError,
      normalized_error: Math.sqrt(best.score)}};
  }

  async record(frame) {
    const [raw, predictions] = await Promise.all([
      this.adsbSource.get('aircraft'), this.adsbSource.get('delayDoppler')]);
    const aircraftByHex = new Map((raw?.aircraft || []).map(item =>
      [normalizedHex(item?.hex), item]));
    const records = frame.recoveries.map(recovery => {
      const match = this.match(frame.timestamp, recovery, aircraftByHex, predictions);
      ++this.counters[match.classification === 'matched' ? 'matched' :
        match.classification === 'ambiguous' ? 'ambiguous' : 'unmatched'];
      return {schema: 'vectorwarp-track-guided-shadow-v1', recorded_at: new Date(this.now()).toISOString(),
        radar_timestamp_ms: frame.timestamp, frequency_hz: this.frequencyHz,
        track_id: recovery.track_id, state: recovery.state, maturity: recovery.maturity,
        prediction: {delay_bin: recovery.delay_bin, delay_km: recovery.delay_km,
          doppler_hz: recovery.doppler_hz}, classification: match.classification,
        adsb: match.adsb};
    });
    const payload = records.map(record => JSON.stringify(record)).join('\n') + '\n';
    await this.fs.promises.mkdir(path.dirname(this.options.path), {recursive: true});
    await this.rotate(Buffer.byteLength(payload));
    await this.fs.promises.appendFile(this.options.path, payload, {mode: 0o600});
    this.recent.push(...records); this.recent = this.recent.slice(-20);
    this.lastWriteAt = this.now(); this.error = null;
  }

  async drain() {
    this.running = true;
    try { while (this.queue.length) await this.record(this.queue.shift()); }
    catch (error) { this.error = error.message; }
    finally { this.running = false; if (this.queue.length) void this.drain(); }
  }
}

function createTrackGuidedShadowRecorder(config, adsbSource, dependencies) {
  return new TrackGuidedShadowRecorder(config, adsbSource, dependencies);
}

module.exports = {TrackGuidedShadowRecorder, createTrackGuidedShadowRecorder};
