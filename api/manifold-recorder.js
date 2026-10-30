'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {compatibilityFingerprint} = require('./manifold-identity');

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
  if (!frame || !['vectorwarp-manifold-evidence-v1', 'vectorwarp-manifold-evidence-v2',
      'vectorwarp-manifold-evidence-v3'].includes(frame.schema) ||
      !finite(frame.timestamp) || !finite(frame.frequency_hz) || frame.frequency_hz <= 0 ||
      !Number.isInteger(frame.channels) || frame.channels < 2 || frame.channels > 8 ||
      !Array.isArray(frame.actual_gain_db)||frame.actual_gain_db.length!==frame.channels||
      frame.actual_gain_db.some(value=>!finite(value))||
      !Array.isArray(frame.reference_weights) || frame.reference_weights.length !== frame.channels ||
      frame.reference_weights.some(value => !Array.isArray(value) || value.length !== 2 ||
        !value.every(finite)) || !Array.isArray(frame.data) || frame.data.length > 65536)
    throw new Error('Invalid manifold evidence frame');
  if (frame.reference_state !== undefined) {
    const state = frame.reference_state;
    if (!Number.isSafeInteger(state?.frames) || state.frames < 0 ||
        !Number.isSafeInteger(state.updates) || state.updates < 0 ||
        !finite(state.coherent_fraction) || state.coherent_fraction < 0 ||
        !finite(state.coherent_gain_db) || !finite(state.scale) || state.scale < 0)
      throw new Error('Invalid reference diagnostic state');
  }
  if (frame.schema !== 'vectorwarp-manifold-evidence-v1') {
    const replay = frame.bearing_replay;
    if (!replay || !['coherent_beam_bank', 'covariance_mvdr'].includes(replay.method) ||
        (replay.reference_anchor !== undefined &&
         !['live_eigenbeam', 'fixed_site'].includes(replay.reference_anchor)) ||
        !Number.isInteger(replay.scan_beams) || replay.scan_beams < 4 || replay.scan_beams > 127 ||
        !Array.isArray(replay.channel_mapping) || replay.channel_mapping.length !== frame.channels ||
        new Set(replay.channel_mapping).size !== frame.channels ||
        replay.channel_mapping.some(value => !Number.isInteger(value) || value < 0 || value >= frame.channels))
      throw new Error('Invalid manifold replay metadata');
  }
  for (const row of frame.data) {
    if (!Number.isInteger(row?.cell) || row.cell < 0 || !finite(row.delay_km) ||
        !finite(row.doppler_hz) || !finite(row.snr_db) || !Array.isArray(row.response) ||
        row.response.length !== frame.channels || row.response.some(value =>
          !Array.isArray(value) || value.length !== 2 || !value.every(finite)))
      throw new Error('Invalid manifold evidence row');
    if (frame.schema !== 'vectorwarp-manifold-evidence-v1' &&
        frame.bearing_replay.method === 'covariance_mvdr' &&
        (!Number.isInteger(row.covariance_samples) || row.covariance_samples < 1 ||
         !Array.isArray(row.covariance) || row.covariance.length !== frame.channels ||
         row.covariance.some(line => !Array.isArray(line) || line.length !== frame.channels ||
           line.some(value => !Array.isArray(value) || value.length !== 2 || !value.every(finite)))))
      throw new Error('Invalid manifold covariance evidence');
    if (row.detection_context !== undefined) {
      const context = row.detection_context, cfar = context?.cfar;
      if (!context || context.same_iq !== true || !Array.isArray(context.nuisance) ||
          context.nuisance.some(item => !Array.isArray(item?.response) ||
            item.response.length !== frame.channels || item.response.some(value =>
              !Array.isArray(value) || value.length !== 2 || !value.every(finite)) ||
            !finite(item.baseline_statistic) ||
            typeof item.baseline_detected !== 'boolean') ||
          !finite(context.target?.baseline_statistic) ||
          typeof context.target.baseline_detected !== 'boolean' ||
          !finite(cfar?.pfa) || cfar.pfa <= 0 || cfar.pfa >= 1 ||
          !Number.isInteger(cfar.training_cells) || cfar.training_cells < 1 ||
          !finite(cfar.effective_trials) || cfar.effective_trials < 1 ||
          !finite(cfar.baseline_threshold) ||
          !Number.isInteger(row.covariance_samples) || row.covariance_samples < 1 ||
          !Array.isArray(row.covariance) || row.covariance.length !== frame.channels ||
          row.covariance.some(line => !Array.isArray(line) || line.length !== frame.channels ||
            line.some(value => !Array.isArray(value) || value.length !== 2 || !value.every(finite))))
        throw new Error('Invalid same-IQ detection context');
    }
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

function fingerprintDescriptor(config) {
  const options = config.process?.bearing?.manifold_recorder || {};
  const device = config.capture?.device || {}, heimdall = device.heimdall || {};
  const channels = device.surveillance_channels;
  let requestedChannelGains = null, requestedCaptureChannelGains;
  if (Array.isArray(heimdall.channel_gains)) {
    if (Array.isArray(channels) && heimdall.channel_gains.length === device.channel_count &&
        channels.every(channel => Number.isInteger(channel) && channel >= 0 &&
          channel < heimdall.channel_gains.length)) {
      requestedCaptureChannelGains = [...heimdall.channel_gains];
      requestedChannelGains = channels.map(channel => heimdall.channel_gains[channel]);
    } else if (Array.isArray(channels) && heimdall.channel_gains.length === channels.length) {
      requestedChannelGains = [...heimdall.channel_gains];
    }
  }
  return {frequency_hz: config.capture?.fc, sample_rate_hz: config.capture?.fs,
    receiver_gain_v2: {requested_common_gain: heimdall.gain ?? 'keep',
      requested_channel_gains: requestedChannelGains,
      ...(requestedCaptureChannelGains ?
        {requested_capture_channel_gains: requestedCaptureChannelGains} : {})},
    surveillance_channels: channels,
    array_geometry: device.array_geometry,
    receiver: config.location?.rx, transmitter: config.location?.tx,
    bearing_replay_v1: {method: config.process?.bearing?.method,
      reference_anchor: config.process?.bearing?.reference_anchor,
      scan_beams: config.process?.bearing?.scan_beams,
      guard_radius: config.process?.bearing?.guard_radius,
      training_radius: config.process?.bearing?.training_radius,
      diagonal_loading: config.process?.bearing?.diagonal_loading,
      channel_mapping: config.process?.bearing?.channel_mapping},
    recorder_selection_v2: {min_snr_db: options.min_snr_db,
      min_elevation_deg: options.min_elevation_deg,
      max_elevation_deg: options.max_elevation_deg,
      max_delay_error_km: options.max_delay_error_km,
      max_doppler_error_hz: options.max_doppler_error_hz,
      max_truth_age_s: options.max_truth_age_s}};
}

function siteFingerprint(config) {
  return crypto.createHash('sha256').update(JSON.stringify(fingerprintDescriptor(config))).digest('hex');
}

function manifoldIdentity(config, actualGainDb, frequencyHz = config.capture?.fc,
  sampleRateHz = config.capture?.fs) {
  const device = config.capture?.device || {}, channels = device.surveillance_channels;
  const heimdall = device.heimdall || {}, geometry = device.array_geometry || {};
  if (!finite(frequencyHz) || frequencyHz <= 0 || !finite(sampleRateHz) || sampleRateHz <= 0 ||
      !Array.isArray(channels) || !channels.length ||
      !Array.isArray(actualGainDb) || actualGainDb.length !== channels.length ||
      actualGainDb.some(value => !finite(value)) || !finite(geometry.x_axis_bearing_deg_true) ||
      !Array.isArray(geometry.elements)) throw new Error('Invalid measured manifold identity');
  let requested;
  if (Array.isArray(heimdall.channel_gains) && heimdall.channel_gains.length === device.channel_count)
    requested = channels.map(channel => heimdall.channel_gains[channel]);
  else if (finite(heimdall.gain) && heimdall.gain >= 0)
    requested = channels.map(() => heimdall.gain);
  else throw new Error('Measured manifold identity requires explicit fixed requested gains');
  if (requested.some(value => !finite(value) || value < 0 || value > 50))
    throw new Error('Invalid requested gain in measured manifold identity');
  const byChannel = new Map(geometry.elements.map(element => [element?.daq_channel, element]));
  const positions = channels.map(channel => byChannel.get(channel)?.position);
  if (positions.some(position => !Array.isArray(position) || position.length !== 3 ||
      position.some(value => !finite(value)))) throw new Error('Invalid geometry in measured manifold identity');
  const location = value => [value?.latitude,value?.longitude,value?.altitude];
  const receiver=location(config.location?.rx),transmitter=location(config.location?.tx);
  if(receiver.some(value=>!finite(value))||transmitter.some(value=>!finite(value)))
    throw new Error('Invalid fixed illuminator identity');
  const identity = {frequency_hz: frequencyHz, sample_rate_hz: sampleRateHz,
    channel_mapping: [...channels],
    requested_gain_db: requested, actual_gain_db: [...actualGainDb], positions_m: positions,
    x_axis_bearing_deg_true: geometry.x_axis_bearing_deg_true,
    receiver_location:receiver,transmitter_location:transmitter};
  identity.compatibility_fingerprint = compatibilityFingerprint(identity);
  return identity;
}

class ManifoldRecorder {
  constructor(config, adsbSource, {fileSystem = fs} = {}) {
    this.config = config;
    this.options = {...(config.process?.bearing?.manifold_recorder || {})};
    if (!finite(this.options.max_truth_age_s)) this.options.max_truth_age_s = 3;
    if (!finite(this.options.max_elevation_deg)) this.options.max_elevation_deg = 15;
    this.adsbSource = adsbSource;
    this.fs = fileSystem;
    this.latest = null;
    this.running = false;
    const automatic = this.options.enabled === undefined || this.options.enabled === 'auto';
    this.state = {enabled: this.options.enabled === true || this.options.enabled === 'on' ||
      (automatic && automaticallyEligible(config)),
      automatic, frames: 0, samples: 0,
      unmatched: 0, staleTruth: 0, elevationRejected: 0, ambiguous: 0,
      lastWriteAt: null, error: null, path: this.options.path || null,
      referencePath: this.options.path ? `${this.options.path}.reference.jsonl` : null,
      referenceUpdates: 0, referenceSamples: 0, lastReferenceAt: null,
      maxReferencePhaseStepDeg: null, maxReferenceAmplitudeStepDb: null};
    this.fingerprint = siteFingerprint(config);
    this.sitePrepared = false;
    this.lastReferenceUpdate = null;
    this.lastRelativeReference = null;
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

  async rotate(bytes, filename = this.options.path) {
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

  async archiveSiteFile(filename) {
    if (this.options.max_files === 1) {
      try { await this.fs.promises.unlink(filename); }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
      return;
    }
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
      await this.archiveSiteFile(filename);
      await this.archiveSiteFile(this.state.referencePath);
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

  relativeReference(weights) {
    let anchor = 0;
    const magnitudes = weights.map(value => Math.hypot(value[0], value[1]));
    if (!(magnitudes[anchor] > 1e-12)) anchor = magnitudes.indexOf(Math.max(...magnitudes));
    if (anchor < 0 || !(magnitudes[anchor] > 1e-12)) return null;
    const [ar, ai] = weights[anchor], denominator = ar * ar + ai * ai;
    return {anchor_channel: anchor, channels: weights.map(([real, imaginary]) => {
      const ratioReal = (real * ar + imaginary * ai) / denominator;
      const ratioImag = (imaginary * ar - real * ai) / denominator;
      return {amplitude_db: 20 * Math.log10(Math.max(1e-15, Math.hypot(ratioReal, ratioImag))),
        phase_deg: degrees(Math.atan2(ratioImag, ratioReal))};
    })};
  }

  async recordReference(frame) {
    const state = frame.reference_state;
    if (!state || state.updates === this.lastReferenceUpdate) return;
    const relative = this.relativeReference(frame.reference_weights);
    if (!relative) return;
    let maxPhaseStep = null, maxAmplitudeStep = null;
    if (this.lastRelativeReference?.anchor_channel === relative.anchor_channel) {
      maxPhaseStep = 0; maxAmplitudeStep = 0;
      for (let index = 0; index < relative.channels.length; ++index) {
        const current = relative.channels[index], prior = this.lastRelativeReference.channels[index];
        const phase = Math.abs(((current.phase_deg - prior.phase_deg + 540) % 360) - 180);
        maxPhaseStep = Math.max(maxPhaseStep, phase);
        maxAmplitudeStep = Math.max(maxAmplitudeStep,
          Math.abs(current.amplitude_db - prior.amplitude_db));
      }
    }
    const record = {schema: 'vectorwarp-reference-drift-v1',
      recorded_at: new Date().toISOString(), radar_timestamp_ms: frame.timestamp,
      frequency_hz: frame.frequency_hz, sample_rate_hz: frame.sample_rate_hz,
      config_fingerprint: this.fingerprint,
      receiver_gain_v2: fingerprintDescriptor(this.config).receiver_gain_v2,
      reference_state: state,
      relative_eigenvector: relative,
      change_from_previous: {max_phase_step_deg: maxPhaseStep,
        max_amplitude_step_db: maxAmplitudeStep}};
    const payload = JSON.stringify(record) + '\n';
    await this.fs.promises.mkdir(path.dirname(this.state.referencePath), {recursive: true});
    await this.rotate(Buffer.byteLength(payload), this.state.referencePath);
    await this.fs.promises.appendFile(this.state.referencePath, payload, {mode: 0o600});
    this.lastReferenceUpdate = state.updates;
    this.lastRelativeReference = relative;
    ++this.state.referenceSamples;
    this.state.referenceUpdates = state.updates;
    this.state.lastReferenceAt = Date.now();
    this.state.maxReferencePhaseStepDeg = maxPhaseStep;
    this.state.maxReferenceAmplitudeStepDb = maxAmplitudeStep;
  }

  async record(frame) {
    await this.prepareSite();
    ++this.state.frames;
    await this.recordReference(frame);
    if (!frame.data.length) return;
    const exactTiming = frame.eventTime?.exact === true &&
      frame.eventTime.uncertaintyBounded === true && frame.eventTime.uncertaintyNs === 0;
    const configuredUncertainty = this.config.truth?.adsb?.provisional_event_time_uncertainty_s;
    const timeUncertaintyS = exactTiming ? 0 :
      (finite(configuredUncertainty) ? Math.max(0, Math.min(5, configuredUncertainty)) : 1);
    const identity = manifoldIdentity(this.config, frame.actual_gain_db, frame.frequency_hz,
      frame.sample_rate_hz);
    const identityFingerprint = crypto.createHash('sha256').update(JSON.stringify({
      configuration: fingerprintDescriptor(this.config), identity})).digest('hex');
    const [raw, delayDoppler] = await Promise.all([
      this.adsbSource.get('aircraft'), this.adsbSource.get('delayDoppler')]);
    const aircraftByHex = new Map((raw.aircraft || []).map(item => [item.hex, item]));
    const candidatesByEvidence = new Map();
    const transmitter = this.config.location?.tx;
    const direct = azimuthElevation(this.config.location?.rx, {lat: transmitter?.latitude,
      lon: transmitter?.longitude, alt_geom: finite(transmitter?.altitude) ? transmitter.altitude / .3048 : NaN});
    if (!direct) throw new Error('Invalid receiver/transmitter geometry for manifold recording');
    for (const [icao, truth] of Object.entries(delayDoppler || {})) {
      const aircraft = aircraftByHex.get(icao);
      const direction = azimuthElevation(this.config.location?.rx, aircraft);
      if (!aircraft || !direction) continue;
      if (!finite(truth.timestamp) ||
          Math.abs(frame.timestamp / 1000 - truth.timestamp) > this.options.max_truth_age_s) {
        ++this.state.staleTruth;
        continue;
      }
      if (direction.elevationDeg < this.options.min_elevation_deg ||
          direction.elevationDeg > this.options.max_elevation_deg) {
        ++this.state.elevationRejected;
        continue;
      }
      for (let index = 0; index < frame.data.length; ++index) {
        const evidence = frame.data[index];
        if (evidence.snr_db < this.options.min_snr_db) continue;
        const delayError = evidence.delay_km - truth.delay;
        const dopplerError = evidence.doppler_hz - truth.doppler;
        const delayGate = this.options.max_delay_error_km +
          (finite(truth.delay_rate_km_s) ? Math.abs(truth.delay_rate_km_s) * timeUncertaintyS : 0);
        const dopplerGate = this.options.max_doppler_error_hz +
          (finite(truth.doppler_rate_hz_s) ? Math.abs(truth.doppler_rate_hz_s) * timeUncertaintyS : 0);
        const score = (delayError / delayGate) ** 2 + (dopplerError / dopplerGate) ** 2;
        if (score <= 1) {
          const candidate = {score, index, icao, truth, aircraft,
            direction, delayError, dopplerError, delayGate, dopplerGate};
          if (!candidatesByEvidence.has(index)) candidatesByEvidence.set(index, []);
          candidatesByEvidence.get(index).push(candidate);
        }
      }
    }
    const candidates = [];
    for (const alternatives of candidatesByEvidence.values()) {
      alternatives.sort((a, b) => a.score - b.score);
      if (alternatives.length > 1 &&
          alternatives[1].score <= alternatives[0].score * 1.15 + .05) {
        ++this.state.ambiguous;
        continue;
      }
      candidates.push(alternatives[0]);
    }
    candidates.sort((a, b) => a.score - b.score);
    const usedEvidence = new Set(), usedAircraft = new Set(), records = [];
    const orientation = this.config.capture?.device?.array_geometry?.x_axis_bearing_deg_true;
    for (const candidate of candidates) {
      if (usedEvidence.has(candidate.index) || usedAircraft.has(candidate.icao)) continue;
      usedEvidence.add(candidate.index); usedAircraft.add(candidate.icao);
      const evidence = frame.data[candidate.index];
      const receiverGain = {...fingerprintDescriptor(this.config).receiver_gain_v2,
        actual_channel_gains_db: [...identity.actual_gain_db]};
      records.push({schema: 'vectorwarp-manifold-sample-v2', recorded_at: new Date().toISOString(),
        radar_timestamp_ms: frame.timestamp, frequency_hz: frame.frequency_hz,
        sample_rate_hz: frame.sample_rate_hz, channels: frame.channels,
        config_fingerprint: identityFingerprint,
        manifold_identity: identity,
        receiver_gain_v2: receiverGain,
        icao: candidate.icao,
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
        configuration: {frequency_hz: frame.frequency_hz,
          sample_rate_hz: frame.sample_rate_hz,
          config_fingerprint: identityFingerprint,
          receiver_gain_v2: receiverGain},
        support: {adsb_label_only: true, covariance_samples: evidence.covariance_samples || 0,
          local_nuisance_cells: evidence.detection_context?.nuisance?.length || 0},
        uncertainty: {truth_age_s: Math.abs(frame.timestamp / 1000 - candidate.truth.timestamp),
          exact_event_time: exactTiming, event_time_uncertainty_s: timeUncertaintyS,
          event_time_source: frame.eventTime?.source || 'unknown',
          event_time_uncertainty_model: exactTiming ? 'exact' : 'bounded-configured-nuisance-radius',
          gate_delay_km: candidate.delayGate, gate_doppler_hz: candidate.dopplerGate,
          delay_error_km: candidate.delayError, doppler_error_hz: candidate.dopplerError,
          normalized_match_error: Math.sqrt(candidate.score)},
        direct_path_local_azimuth_deg: normalize(orientation - direct.bearingDeg),
        response: evidence.response, reference_weights: frame.reference_weights,
        ...(frame.reference_state ? {reference_state: frame.reference_state} : {}),
        ...(frame.bearing_replay ? {bearing_replay: frame.bearing_replay} : {}),
        ...(evidence.covariance ? {covariance: evidence.covariance,
          covariance_samples: evidence.covariance_samples} : {})});
      if (evidence.covariance || evidence.detection_context) {
        records.at(-1).local_context = {
          ...(evidence.covariance ? {covariance: evidence.covariance,
            covariance_samples: evidence.covariance_samples} : {}),
          ...(evidence.detection_context ? {detection: evidence.detection_context} : {})};
      }
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

module.exports = {azimuthElevation, validateEvidence, automaticallyEligible, fingerprintDescriptor, siteFingerprint, manifoldIdentity,
  ManifoldRecorder, createManifoldRecorder};
