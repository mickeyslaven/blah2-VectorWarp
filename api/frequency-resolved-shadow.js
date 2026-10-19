'use strict';

const fs = require('fs');
const path = require('path');
const {Worker} = require('worker_threads');
const {analysisDescriptor, analysisFingerprint} = require('./frequency-resolved-analysis');

const magnitude2 = value => value[0] * value[0] + value[1] * value[1];
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
  FrequencyResolvedShadow, createFrequencyResolvedShadow};
