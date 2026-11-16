'use strict';

const fs = require('fs');
const path = require('path');
const {Worker} = require('worker_threads');
const {analysisDescriptor, analysisFingerprint} = require('./frequency-resolved-analysis');
const {processingCapabilities} = require('./processing-capabilities');

const magnitude2 = value => value[0] * value[0] + value[1] * value[1];
const add = (left, right) => [left[0] + right[0], left[1] + right[1]];
const multiply = (left, right) => [left[0] * right[0] - left[1] * right[1],
  left[0] * right[1] + left[1] * right[0]];
const median = values => {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b), middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};

function vectorCorrelation(left, right) {
  if (!Array.isArray(left) || !Array.isArray(right) || left.length !== right.length) return null;
  let real = 0, imaginary = 0, leftPower = 0, rightPower = 0;
  for (let index = 0; index < left.length; ++index) {
    const a = left[index], b = right[index];
    if (!Array.isArray(a) || !Array.isArray(b)) return null;
    real += a[0] * b[0] + a[1] * b[1];
    imaginary += a[0] * b[1] - a[1] * b[0];
    leftPower += magnitude2(a); rightPower += magnitude2(b);
  }
  return Math.hypot(real, imaginary) / Math.sqrt(leftPower * rightPower);
}

function modelStability(prior, current) {
  if (prior?.config_fingerprint !== current?.config_fingerprint ||
      !Array.isArray(prior.subbands) || prior.subbands.length !== current?.subbands?.length) return null;
  const principal = current.subbands.map((row, index) =>
    vectorCorrelation(prior.subbands[index].principal_response, row.principal_response));
  const mvdr = current.subbands.map((row, index) => vectorCorrelation(
    prior.subbands[index].direct_constrained_mvdr?.weights,
    row.direct_constrained_mvdr?.weights)).filter(value => value !== null);
  if (principal.some(value => value === null)) return null;
  return {principal_response_correlation: {min: Math.min(...principal), median: median(principal)},
    direct_constrained_weight_correlation: mvdr.length ?
      {min: Math.min(...mvdr), median: median(mvdr)} : null};
}

function phaseOnlyEqualizationCandidate(prior, current, {minimumHistory = 8,
  minimumCorrelation = .98, minimumRank1Fraction = .95} = {}) {
  if (prior?.config_fingerprint !== current?.config_fingerprint ||
      !Array.isArray(prior?.subbands) || !Array.isArray(current?.subbands) ||
      prior.subbands.length !== current.subbands.length || !current.subbands.length)
    return null;
  const correlations = current.subbands.map((row, band) =>
    vectorCorrelation(prior.subbands[band]?.principal_response,
      row?.principal_response));
  if (correlations.some(value => value === null)) return null;
  const history = [...(Array.isArray(prior.history) ? prior.history : []), {
    rank1_fraction: current.summary?.rank1_fraction,
    stability_from_previous: modelStability(prior, current)}];
  const stableTail = history.slice(-minimumHistory);
  const historyStable = stableTail.length >= minimumHistory && stableTail.every(row =>
    row?.rank1_fraction?.min >= minimumRank1Fraction &&
    row?.stability_from_previous?.principal_response_correlation?.min >=
      minimumCorrelation);
  const subbands = current.subbands.map((row, band) => {
    const previous = prior.subbands[band].principal_response;
    const response = row.principal_response;
    if (!Array.isArray(response) || response.length < 2 ||
        !Array.isArray(previous) || previous.length !== response.length)
      throw new Error('Frequency equalization response dimensions changed');
    const weights = response.map((value, channel) => {
      const averaged = add(value, previous[channel]);
      const size = Math.sqrt(magnitude2(averaged));
      if (!(size > 1e-12) || !Number.isFinite(size))
        throw new Error('Frequency equalization response contains a null channel');
      // Phase-only correction has exactly unity gain, so it cannot amplify
      // receiver noise. Amplitude whitening remains the downstream bounded
      // noise-whitened beam-bank's responsibility.
      return [averaged[0] / size, -averaged[1] / size];
    });
    const coherentPower = values => magnitude2(values.reduce((sum, value) =>
      add(sum, value), [0, 0]));
    const corrected = response.map((value, channel) => multiply(weights[channel], value));
    return {offset_hz: row.offset_hz, weights,
      current_prior_correlation: correlations[band], noise_gain: 1,
      measured_direct_coherence_gain_db: 10 * Math.log10(
        coherentPower(corrected) / Math.max(coherentPower(response), 1e-30))};
  });
  return {schema: 'vectorwarp-frequency-phase-equalization-candidate-v1',
    intended_use: 'pre-caf-frequency-domain-shadow-replay-only',
    config_fingerprint: current.config_fingerprint, phase_only: true,
    maximum_noise_gain: 1, minimum_history: minimumHistory,
    minimum_rank1_fraction: minimumRank1Fraction,
    minimum_prior_correlation: minimumCorrelation,
    observed_prior_correlation: {min: Math.min(...correlations),
      median: median(correlations)}, stable_history_observations: stableTail.length,
    ready_for_shadow_replay: historyStable, production_authorized: false,
    rejection_reason: historyStable ? null :
      'frequency response has not met the bounded multi-observation stability gate',
    subbands};
}

function requested(config) {
  const value = config.process?.bearing?.frequency_resolved_shadow?.enabled;
  return value === true || value === 'on' || value === 'auto';
}

class FrequencyResolvedShadow {
  constructor(config, {WorkerClass = Worker, fileSystem = fs, initialDelayMs = 10000} = {}) {
    this.config = config; this.WorkerClass = WorkerClass; this.fs = fileSystem;
    this.options = {...(config.process?.bearing?.frequency_resolved_shadow || {})};
    this.timer = null; this.worker = null; this.initialDelayMs = initialDelayMs;
    this.state = {enabled: false, active: false, analyses: 0, error: null,
      lastAnalysisAt: null, result: null};
    if (!requested(config)) return;
    try {
      const capabilities = processingCapabilities(config);
      if (!capabilities.liveMchq || !capabilities.coherent ||
          !capabilities.confirmedGeometry || !capabilities.referenceGeometry)
        throw new Error('Frequency-resolved shadow requires a live MCHQ endpoint and confirmed coherent reference geometry');
      this.descriptor = analysisDescriptor(config);
      this.fingerprint = analysisFingerprint(this.descriptor);
      this.outputPath = this.options.path ||
        `${config.process?.bearing?.manifold_recorder?.path || '/var/lib/vectorwarp-api/calibration/manifold-samples.jsonl'}.frequency-resolved.json`;
      this.state.enabled = true;
    } catch (error) { this.state.error = error.message; }
  }

  status() { return {...this.state, fingerprint: this.fingerprint, path: this.outputPath}; }

  start() {
    if (this.paused || !this.state.enabled || this.timer || this.worker) return;
    this.timer = setTimeout(() => { this.timer = null; void this.run(); }, this.initialDelayMs);
    this.timer.unref();
  }

  schedule() {
    if (this.paused || !this.state.enabled || this.timer) return;
    const delay = (this.options.interval_s ?? 300) * 1000;
    this.timer = setTimeout(() => { this.timer = null; void this.run(); }, delay);
    this.timer.unref();
  }

  async persist(result) {
    const payload = `${JSON.stringify(result, null, 2)}\n`;
    await this.fs.promises.mkdir(path.dirname(this.outputPath), {recursive: true});
    const temporary = `${this.outputPath}.${process.pid}.tmp`;
    await this.fs.promises.writeFile(temporary, payload, {mode: 0o600});
    await this.fs.promises.rename(temporary, this.outputPath);
  }

  async prior() {
    try { return JSON.parse(await this.fs.promises.readFile(this.outputPath, 'utf8')); }
    catch (_) { return null; }
  }

  async run() {
    if (this.paused || !this.state.enabled || this.worker) return this.status();
    this.state = {...this.state, active: true, error: null};
    const workerPath = path.join(__dirname, 'frequency-resolved-shadow-worker.js');
    const heimdall = this.config.capture.device.heimdall || {};
    try {
      const result = await new Promise((resolve, reject) => {
        const worker = this.worker = new this.WorkerClass(workerPath, {workerData: {
          descriptor: this.descriptor, options: {host: heimdall.host || '127.0.0.1', port: heimdall.port,
            capture_seconds: this.options.capture_seconds ?? .25, timeout_ms: this.options.timeout_ms ?? 15000}}});
        let settled = false;
        const done = callback => value => { if (!settled) { settled = true; this.worker = null; callback(value); } };
        worker.once('message', done(message => message.ok ? resolve(message.result) : reject(new Error(message.error))));
        worker.once('error', done(reject));
        worker.once('exit', code => { if (!settled) done(reject)(new Error(`Frequency-resolved worker exited ${code}`)); });
      });
      if (this.paused) return this.status();
      if (result.config_fingerprint !== this.fingerprint)
        throw new Error('Frequency-resolved worker returned a different site fingerprint');
      const prior = await this.prior();
      result.sequence = prior?.config_fingerprint === this.fingerprint &&
        Number.isSafeInteger(prior.sequence) ? prior.sequence + 1 : 1;
      result.stability_from_previous = modelStability(prior, result);
      result.phase_equalization_candidate =
        phaseOnlyEqualizationCandidate(prior, result);
      const summary = {sequence: result.sequence, generated_at: result.generated_at,
        rank1_fraction: result.summary?.rank1_fraction,
        ideal_correlation: result.summary?.ideal_correlation,
        mvdr_noise_gain: result.summary?.mvdr_noise_gain,
        stability_from_previous: result.stability_from_previous};
      result.history = [...(prior?.config_fingerprint === this.fingerprint && Array.isArray(prior.history) ?
        prior.history : []), summary].slice(-48);
      await this.persist(result);
      this.state = {...this.state, active: false, analyses: this.state.analyses + 1,
        lastAnalysisAt: Date.now(), result, error: null};
    } catch (error) { this.state = {...this.state, active: false, error: error.message}; }
    finally { this.schedule(); }
    return this.status();
  }

  async pause() {
    this.paused = true;
    clearTimeout(this.timer); this.timer = null;
    if (this.worker) await this.worker.terminate();
    this.state = {...this.state, active: false};
  }

  resume() { this.paused = false; this.start(); }
}

function createFrequencyResolvedShadow(config, dependencies) {
  return new FrequencyResolvedShadow(config, dependencies);
}

module.exports = {requested, vectorCorrelation, modelStability,
  phaseOnlyEqualizationCandidate, FrequencyResolvedShadow,
  createFrequencyResolvedShadow};
