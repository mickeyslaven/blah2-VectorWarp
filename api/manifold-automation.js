'use strict';

const fs = require('fs');
const path = require('path');
const {Worker} = require('worker_threads');
const {fitManifold, labelPasses, passIsHoldout, summarizeCoverage} = require('./manifold-fitter');
const {bestBearing, bestMeasuredBearing, circularError} = require('./bearing-diagnostic');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const median = values => {
  const sorted = [...values].sort((a, b) => a - b), middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};
const normalize = value => ((value % 360) + 360) % 360;

const DEFAULT_GATES = Object.freeze({minIndependentPasses: 40, minUniqueAircraft: 12,
  minCoveredSectors: 6, minPassesPerSector: 5, minAircraftPerSector: 3,
  minHoldoutPasses: 12, minHoldoutSectors: 4, minimumImprovementDeg: 5,
  maximumErrorRatio: .85, maximumMedianErrorDeg: 45, maximumSectorRegressionDeg: 8});

function passBalancedComparison(samples, config, manifold) {
  const mapping = config.process?.bearing?.channel_mapping ||
    Array.from({length: samples[0]?.channels || 0}, (_, index) => index);
  const byPass = new Map();
  for (const sample of labelPasses(samples).filter(row => passIsHoldout(row._pass_id))) try {
    const truth = sample.truth.bearing_deg_true;
    if (!finite(truth)) continue;
    const ideal = bestBearing(sample, config, {mapping});
    const measured = bestMeasuredBearing(sample, config, manifold);
    if (!byPass.has(sample._pass_id)) byPass.set(sample._pass_id, []);
    byPass.get(sample._pass_id).push({ideal: circularError(ideal, truth),
      measured: circularError(measured, truth),
      sector: Math.floor(normalize(sample.truth.local_azimuth_deg) / 45) % 8});
  } catch (_) { /* A malformed replay row cannot promote a model. */ }
  const passes = [...byPass.entries()].map(([id, rows]) => {
    const counts = new Map();
    for (const row of rows) counts.set(row.sector, (counts.get(row.sector) || 0) + 1);
    const sector = [...counts.entries()].sort((left, right) => right[1] - left[1] || left[0] - right[0])[0][0];
    return {id, ideal: median(rows.map(row => row.ideal)),
      measured: median(rows.map(row => row.measured)), sector};
  });
  const summarize = key => ({median_error_deg: passes.length ? median(passes.map(row => row[key])) : null,
    within_45_fraction: passes.length ? passes.filter(row => row[key] <= 45).length / passes.length : null});
  const sectors = [];
  for (let sector = 0; sector < 8; ++sector) {
    const rows = passes.filter(row => row.sector === sector);
    sectors.push({sector, passes: rows.length,
      ideal_median_error_deg: rows.length ? median(rows.map(row => row.ideal)) : null,
      measured_median_error_deg: rows.length ? median(rows.map(row => row.measured)) : null});
  }
  return {passes: passes.length, sectors: sectors.filter(row => row.passes).length,
    ideal: summarize('ideal'), measured: summarize('measured'), by_sector: sectors};
}

function evaluateManifoldCandidate(samples, config, options = {}) {
  const beams = options.beams || config.process?.bearing?.scan_beams || 60;
  const manifold = fitManifold(samples, {beams, minSamples: options.minSamples || 8,
    minPasses: options.minPasses || 3, minCoverage: options.minCoverage || .6,
    maxGapDeg: options.maxGapDeg || 90,
    maxElevationDeg: config.process?.bearing?.manifold_recorder?.max_elevation_deg ?? 15,
    maxDelayErrorKm: config.process?.bearing?.manifold_recorder?.max_delay_error_km ?? .25,
    maxDopplerErrorHz: config.process?.bearing?.manifold_recorder?.max_doppler_error_hz ?? 5,
    maxTruthAgeS: config.process?.bearing?.manifold_recorder?.max_truth_age_s ?? 3});
  const gates = {...DEFAULT_GATES, ...(options.gates || {})};
  const coverage = summarizeCoverage(samples);
  const covered = coverage.filter(row => row.passes >= gates.minPassesPerSector &&
    row.aircraft >= gates.minAircraftPerSector);
  const comparison = passBalancedComparison(samples, config, manifold);
  const regressions = comparison.by_sector.filter(row => row.passes >= 2 &&
    row.measured_median_error_deg - row.ideal_median_error_deg > gates.maximumSectorRegressionDeg);
  const failures = [];
  const calibration = manifold.calibration;
  if (calibration.independent_passes < gates.minIndependentPasses)
    failures.push(`Need ${gates.minIndependentPasses} independent passes; have ${calibration.independent_passes}.`);
  if (calibration.unique_aircraft < gates.minUniqueAircraft)
    failures.push(`Need ${gates.minUniqueAircraft} aircraft; have ${calibration.unique_aircraft}.`);
  if (covered.length < gates.minCoveredSectors)
    failures.push(`Need ${gates.minCoveredSectors}/8 independently covered sectors; have ${covered.length}/8.`);
  if (comparison.passes < gates.minHoldoutPasses)
    failures.push(`Need ${gates.minHoldoutPasses} withheld passes; have ${comparison.passes}.`);
  if (comparison.sectors < gates.minHoldoutSectors)
    failures.push(`Need withheld evidence in ${gates.minHoldoutSectors} sectors; have ${comparison.sectors}.`);
  if (!finite(comparison.measured.median_error_deg) ||
      comparison.measured.median_error_deg > gates.maximumMedianErrorDeg)
    failures.push(`Measured holdout median must be at most ${gates.maximumMedianErrorDeg} degrees.`);
  if (finite(comparison.ideal.median_error_deg) && finite(comparison.measured.median_error_deg) &&
      comparison.measured.median_error_deg > comparison.ideal.median_error_deg - gates.minimumImprovementDeg &&
      comparison.measured.median_error_deg > comparison.ideal.median_error_deg * gates.maximumErrorRatio)
    failures.push('Measured manifold did not materially beat the default model on withheld passes.');
  if (finite(comparison.ideal.within_45_fraction) && finite(comparison.measured.within_45_fraction) &&
      comparison.measured.within_45_fraction < comparison.ideal.within_45_fraction)
    failures.push('Measured manifold reduced the withheld within-45-degree pass rate.');
  if (regressions.length)
    failures.push(`Measured manifold regressed ${regressions.length} independently tested sector(s).`);
  const evaluation = {schema: 'vectorwarp-manifold-evaluation-v1', evaluated_at: new Date().toISOString(),
    qualified: failures.length === 0, failures, gates, coverage, comparison,
    training: {samples: calibration.input_samples, passes: calibration.independent_passes,
      aircraft: calibration.unique_aircraft, observed_bins: calibration.observed_bins,
      interpolated_bins: calibration.interpolated_bins,
      largest_gap_deg: calibration.largest_observed_gap_deg}};
  manifold.calibration.automatic_evaluation = evaluation;
  return {manifold, evaluation};
}

class ManifoldAutomation {
  constructor(config, recorder, {worker = Worker, intervalMs = 15 * 60 * 1000,
    initialDelayMs = 30000, onQualified = null} = {}) {
    this.config = config; this.recorder = recorder; this.Worker = worker;
    this.intervalMs = intervalMs; this.initialDelayMs = initialDelayMs;
    this.samplePath = config.process?.bearing?.manifold_recorder?.path;
    this.candidatePath = this.samplePath ? `${this.samplePath}.candidate.json` : null;
    this.qualifiedPath = this.samplePath ? `${this.samplePath}.qualified.json` : null;
    this.timer = null; this.running = false; this.lastSignature = null;
    this.onQualified = onQualified;
    this.autoPromote = config.process?.bearing?.manifold_automation?.auto_promote !== false;
    this.state = {enabled: Boolean(this.samplePath && recorder.status().enabled),
      state: this.samplePath && recorder.status().enabled ? 'collecting' : 'disabled',
      candidatePath: this.candidatePath, qualifiedPath: this.qualifiedPath,
      qualified: false, error: null};
  }

  status() { return {...this.state, running: this.running}; }

  start() {
    if (!this.state.enabled || this.timer) return;
    const schedule = delay => {
      this.timer = setTimeout(async () => {
        try { await this.evaluate(); } catch (_) { /* status carries the error */ }
        schedule(this.intervalMs);
      }, delay);
      this.timer.unref();
    };
    schedule(this.initialDelayMs);
  }

  async evaluate({force = false} = {}) {
    if (!this.state.enabled || this.running) return this.status();
    let stat;
    try { stat = await fs.promises.stat(this.samplePath); }
    catch (error) {
      if (error.code === 'ENOENT') return this.status();
      this.state = {...this.state, state: 'error', error: error.message}; return this.status();
    }
    const signature = `${stat.size}:${stat.mtimeMs}`;
    if (!force && signature === this.lastSignature) return this.status();
    this.running = true; this.state = {...this.state, state: 'evaluating', error: null};
    try {
      const result = await new Promise((resolve, reject) => {
        const worker = new this.Worker(path.join(__dirname, 'manifold-automation-worker.js'),
          {workerData: {samplePath: this.samplePath, candidatePath: this.candidatePath,
            qualifiedPath: this.qualifiedPath, config: this.config}});
        worker.once('message', message => message.ok ? resolve(message.result) : reject(new Error(message.error)));
        worker.once('error', reject);
        worker.once('exit', code => { if (code) reject(new Error(`Manifold worker exited ${code}`)); });
      });
      this.lastSignature = signature;
      this.state = {...this.state, ...result,
        state: result.qualified ? 'qualified' : 'collecting', error: null, lastEvaluationAt: Date.now()};
      if (result.qualified && this.autoPromote && this.onQualified) {
        try {
          const promotion = await this.onQualified({path: this.qualifiedPath,
            fingerprint: result.fingerprint, evidence: result.evidence});
          this.state = {...this.state, promotion,
            state: promotion?.active ? 'active' : promotion?.pending ? 'qualified-pending-restart' : 'qualified'};
        } catch (error) {
          this.lastSignature = null;
          this.state = {...this.state, state: 'qualified',
            promotion: {active: false, pending: true, reason: error.message}};
        }
      }
    } catch (error) {
      this.lastSignature = signature;
      this.state = {...this.state, state: 'collecting', qualified: false,
        error: error.message, lastEvaluationAt: Date.now()};
    } finally { this.running = false; }
    return this.status();
  }
}

function createManifoldAutomation(config, recorder, dependencies) {
  return new ManifoldAutomation(config, recorder, dependencies);
}

module.exports = {DEFAULT_GATES, passBalancedComparison, evaluateManifoldCandidate,
  ManifoldAutomation, createManifoldAutomation};
