'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const normalize = value => ((value % 360) + 360) % 360;
const radians = value => value * Math.PI / 180;
const degrees = value => value * 180 / Math.PI;

function azimuthElevation(site, aircraft) {
  if (![site?.latitude, site?.longitude, site?.altitude, aircraft?.lat,
    aircraft?.lon, aircraft?.alt_geom].every(finite)) return null;
  const lat1 = radians(site.latitude), lat2 = radians(aircraft.lat);
  const dlon = radians(aircraft.lon - site.longitude);
  const y = Math.sin(dlon) * Math.cos(lat2);
  const x = Math.cos(lat1) * Math.sin(lat2) -
    Math.sin(lat1) * Math.cos(lat2) * Math.cos(dlon);
  const bearingDeg = normalize(degrees(Math.atan2(y, x)));
  const earth = 6371008.8;
  const a = Math.sin((lat2 - lat1) / 2) ** 2 +
    Math.cos(lat1) * Math.cos(lat2) * Math.sin(dlon / 2) ** 2;
  const groundM = earth * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(Math.max(0, 1 - a)));
  const heightM = aircraft.alt_geom * .3048 - site.altitude;
  const elevationDeg = degrees(Math.atan2(heightM, Math.max(groundM, 1)));
  return finite(bearingDeg) && finite(elevationDeg) ? {bearingDeg, elevationDeg} : null;
}

function validateEvidence(frame) {
  if (!frame || frame.schema !== 'vectorwarp-manifold-evidence-v1' ||
      !finite(frame.timestamp) || !finite(frame.frequency_hz) || frame.frequency_hz <= 0 ||
      !Number.isInteger(frame.channels) || frame.channels < 2 || frame.channels > 8 ||
      !Array.isArray(frame.reference_weights) || frame.reference_weights.length !== frame.channels ||
      frame.reference_weights.some(value => !Array.isArray(value) || value.length !== 2 ||
        !value.every(finite)) || !Array.isArray(frame.data) || frame.data.length > 65536)
    throw new Error('Invalid manifold evidence frame');
  for (const row of frame.data) {
    if (!Number.isInteger(row?.cell) || row.cell < 0 || !finite(row.delay_km) ||
        !finite(row.doppler_hz) || !finite(row.snr_db) || !Array.isArray(row.response) ||
        row.response.length !== frame.channels || row.response.some(value =>
          !Array.isArray(value) || value.length !== 2 || !value.every(finite)))
      throw new Error('Invalid manifold evidence row');
  }
  return frame;
}

function automaticallyEligible(config) {
  const device = config.capture?.device || {}, geometry = device.array_geometry || {};
  const fusion = config.process?.surveillance_fusion?.mode;
  return device.type === 'Kraken' &&
    ['coherent_beam_bank', 'noise_whitened_beam_bank'].includes(fusion) &&
    config.process?.detection?.enable === true && config.truth?.adsb?.enabled === true &&
    geometry.mapping_confirmed === true && geometry.geometry_confirmed === true &&
    geometry.orientation_confirmed === true && finite(geometry.x_axis_bearing_deg_true);
}

class ManifoldRecorder {
  constructor(config, adsbSource, {fileSystem = fs} = {}) {
    this.config = config;
    this.options = config.process?.bearing?.manifold_recorder || {};
    this.adsbSource = adsbSource;
    this.fs = fileSystem;
    this.latest = null;
    this.running = false;
    const automatic = this.options.enabled === undefined || this.options.enabled === 'auto';
    this.state = {enabled: this.options.enabled === true || this.options.enabled === 'on' ||
      (automatic && automaticallyEligible(config)),
      automatic, frames: 0, samples: 0,
      unmatched: 0, lastWriteAt: null, error: null, path: this.options.path || null};
    const fingerprint = {
      frequency_hz: config.capture?.fc, sample_rate_hz: config.capture?.fs,
      surveillance_channels: config.capture?.device?.surveillance_channels,
      array_geometry: config.capture?.device?.array_geometry,
      receiver: config.location?.rx, transmitter: config.location?.tx
    };
    this.fingerprint = crypto.createHash('sha256').update(JSON.stringify(fingerprint)).digest('hex');
    this.sitePrepared = false;
  }

  status() { return {...this.state, queued: this.latest !== null, active: this.running}; }

  accept(text) {
    if (!this.state.enabled) return;
    try { this.latest = validateEvidence(JSON.parse(text)); }
    catch (error) { this.state.error = error.message; return; }
    if (!this.running) void this.drain();
  }

  async drain() {
    this.running = true;
    try {
      while (this.latest) {
        const frame = this.latest;
        this.latest = null;
        await this.record(frame);
      }
    } catch (error) {
      this.state.error = error.message;
    } finally { this.running = false; }
  }

  async rotate(bytes) {
    const filename = this.options.path;
    const limit = this.options.max_file_mb * 1024 * 1024;
    let size = 0;
    try { size = (await this.fs.promises.stat(filename)).size; }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    if (size + bytes <= limit) return;
    if (this.options.max_files === 1) {
      try { await this.fs.promises.unlink(filename); }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
      return;
    }
    for (let i = this.options.max_files - 1; i >= 1; --i) {
      const from = i === 1 ? filename : `${filename}.${i - 1}`;
      const to = `${filename}.${i}`;
      try { await this.fs.promises.rename(from, to); }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
  }

  async prepareSite() {
    if (this.sitePrepared) return;
    const filename = this.options.path, metadata = `${filename}.site.json`;
    let prior = null;
    try { prior = JSON.parse(await this.fs.promises.readFile(metadata, 'utf8')); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    if (!prior) {
      try {
        const text = await this.fs.promises.readFile(filename, 'utf8');
        const lines = text.trim().split('\n');
        if (lines.length && lines[lines.length - 1]) prior = {
          fingerprint: JSON.parse(lines[lines.length - 1]).config_fingerprint};
      } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    if (prior?.fingerprint && prior.fingerprint !== this.fingerprint) {
      if (this.options.max_files === 1) {
        try { await this.fs.promises.unlink(filename); }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
      } else {
        for (let i = this.options.max_files - 2; i >= 1; --i) {
          try { await this.fs.promises.rename(`${filename}.site-previous.${i}`,
            `${filename}.site-previous.${i + 1}`); }
          catch (error) { if (error.code !== 'ENOENT') throw error; }
        }
        try { await this.fs.promises.rename(`${filename}.site-previous`,
          `${filename}.site-previous.1`); }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
        try { await this.fs.promises.rename(filename, `${filename}.site-previous`); }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
      }
      this.state.siteResetAt = Date.now();
    }
    await this.fs.promises.mkdir(path.dirname(filename), {recursive: true});
    const site = JSON.stringify({schema: 'vectorwarp-manifold-site-v1',
      fingerprint: this.fingerprint, started_at: new Date().toISOString()}) + '\n';
    const temporary = `${metadata}.${process.pid}.tmp`;
    await this.fs.promises.writeFile(temporary, site, {mode: 0o600});
    await this.fs.promises.rename(temporary, metadata);
    this.sitePrepared = true;
  }

  async record(frame) {
    await this.prepareSite();
    ++this.state.frames;
    if (!frame.data.length) return;
    const [raw, delayDoppler] = await Promise.all([
      this.adsbSource.get('aircraft'), this.adsbSource.get('delayDoppler')]);
    const aircraftByHex = new Map((raw.aircraft || []).map(item => [item.hex, item]));
    const candidates = [];
    const transmitter = this.config.location?.tx;
    const direct = azimuthElevation(this.config.location?.rx, {lat: transmitter?.latitude,
      lon: transmitter?.longitude, alt_geom: finite(transmitter?.altitude) ? transmitter.altitude / .3048 : NaN});
    if (!direct) throw new Error('Invalid receiver/transmitter geometry for manifold recording');
    for (const [icao, truth] of Object.entries(delayDoppler || {})) {
      const aircraft = aircraftByHex.get(icao);
      const direction = azimuthElevation(this.config.location?.rx, aircraft);
      if (!aircraft || !direction || direction.elevationDeg < this.options.min_elevation_deg) continue;
      for (let index = 0; index < frame.data.length; ++index) {
        const evidence = frame.data[index];
        if (evidence.snr_db < this.options.min_snr_db) continue;
        const delayError = evidence.delay_km - truth.delay;
        const dopplerError = evidence.doppler_hz - truth.doppler;
        const score = (delayError / this.options.max_delay_error_km) ** 2 +
          (dopplerError / this.options.max_doppler_error_hz) ** 2;
        if (score <= 1) candidates.push({score, index, icao, truth, aircraft,
          direction, delayError, dopplerError});
      }
    }
    candidates.sort((a, b) => a.score - b.score);
    const usedEvidence = new Set(), usedAircraft = new Set(), records = [];
    const orientation = this.config.capture?.device?.array_geometry?.x_axis_bearing_deg_true;
    for (const candidate of candidates) {
      if (usedEvidence.has(candidate.index) || usedAircraft.has(candidate.icao)) continue;
      usedEvidence.add(candidate.index); usedAircraft.add(candidate.icao);
      const evidence = frame.data[candidate.index];
      records.push({schema: 'vectorwarp-manifold-sample-v1', recorded_at: new Date().toISOString(),
        radar_timestamp_ms: frame.timestamp, frequency_hz: frame.frequency_hz,
        sample_rate_hz: frame.sample_rate_hz, channels: frame.channels,
        config_fingerprint: this.fingerprint, icao: candidate.icao,
        flight: typeof candidate.aircraft.flight === 'string' ? candidate.aircraft.flight.trim() : '',
        truth: {bearing_deg_true: candidate.direction.bearingDeg,
          elevation_deg: candidate.direction.elevationDeg,
          local_azimuth_deg: normalize(orientation - candidate.direction.bearingDeg),
          delay_km: candidate.truth.delay, doppler_hz: candidate.truth.doppler,
          position_timestamp_s: candidate.truth.timestamp},
        match: {delay_error_km: candidate.delayError,
          doppler_error_hz: candidate.dopplerError, normalized_error: Math.sqrt(candidate.score)},
        detection: {cell: evidence.cell, delay_km: evidence.delay_km,
          doppler_hz: evidence.doppler_hz, snr_db: evidence.snr_db},
        direct_path_local_azimuth_deg: normalize(orientation - direct.bearingDeg),
        response: evidence.response, reference_weights: frame.reference_weights});
    }
    this.state.unmatched += frame.data.length - usedEvidence.size;
    if (!records.length) return;
    const payload = records.map(record => JSON.stringify(record)).join('\n') + '\n';
    await this.fs.promises.mkdir(path.dirname(this.options.path), {recursive: true});
    await this.rotate(Buffer.byteLength(payload));
    await this.fs.promises.appendFile(this.options.path, payload, {mode: 0o600});
    this.state.samples += records.length;
    this.state.lastWriteAt = Date.now();
    this.state.error = null;
  }
}

function createManifoldRecorder(config, adsbSource, dependencies) {
  return new ManifoldRecorder(config, adsbSource, dependencies);
}

module.exports = {azimuthElevation, validateEvidence, automaticallyEligible,
  ManifoldRecorder, createManifoldRecorder};
