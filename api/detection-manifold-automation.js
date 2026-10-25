'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {Worker} = require('worker_threads');
const {buildDetectionCorpus} = require('./detection-manifold-corpus');
const {fitDetectionManifold, denseRuntimeCandidate, responseFor, statistic, cfarThreshold} =
  require('./detection-manifold-fitter');

const finite = value => typeof value === 'number' && Number.isFinite(value);
const median = values => {
  const sorted = [...values].sort((a, b) => a - b), middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};
const normalize = value => ((value % 360) + 360) % 360;
const canonical = value => Array.isArray(value) ? `[${value.map(canonical).join(',')}]` :
  value && typeof value === 'object' ? `{${Object.keys(value).sort().map(key =>
    `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}` : JSON.stringify(value);
const canonicalHash = value => crypto.createHash('sha256').update(canonical(value)).digest('hex');
const DEFAULT_DETECTION_GATES = Object.freeze({minIndependentPasses: 40,
  minValidationPasses: 12, minMissPasses: 6, minCoveredSectors: 6,
  minValidationSectors: 4,
  minProductionPasses: 12, minProductionMissPasses: 6, minProductionSectors: 4,
  minProductionPassesPerSector: 1,
  minFitBins: 6, maxUnsupportedGapDeg: 90, minTargetMarginImprovementDb: 1,
  maximumNuisanceRatio: 1.2, minValidationSupportFraction: 0.8,
  minCfarTrainingCells: 8, minHoldoutNuisanceCells: 100});

function coverage(rows, sectors = 8) {
  const result = Array.from({length: sectors}, (_, sector) =>
    ({sector, passes: new Set(), samples: 0}));
  for (const row of rows) {
    const sector = Math.floor(normalize(row.truth.local_azimuth_deg) / 360 * sectors) % sectors;
    ++result[sector].samples; result[sector].passes.add(row._pass_id);
  }
  return result.map(row => ({sector: row.sector, samples: row.samples, passes: row.passes.size}));
}

function largestGap(covered, sectors = 8) {
  if (covered.length < 2) return 360;
  let gap = 0;
  for (let index = 0; index < covered.length; ++index)
    gap = Math.max(gap, ((covered[(index + 1) % covered.length] - covered[index] + sectors) % sectors) * 360 / sectors);
  return gap;
}

function summarizeDetectionManifoldProgress(rawSamples, config, options = {}) {
  const gates = {...DEFAULT_DETECTION_GATES, ...(options.gates || {})};
  const corpus = buildDetectionCorpus(rawSamples, {corpusSha256: options.corpusSha256});
  const allCoverage = coverage(corpus.accepted), validationCoverage = coverage(corpus.validation);
  const covered = allCoverage.filter(row => row.passes >= 2).map(row => row.sector);
  const validationCovered = validationCoverage.filter(row => row.passes).map(row => row.sector);
  const fitSupport = new Map();
  for (const row of corpus.training) {
    const key = `${Math.floor(normalize(row.truth.local_azimuth_deg) / 45)}:${Math.floor(row.truth.elevation_deg / 5)}`;
    if (!fitSupport.has(key)) fitSupport.set(key, new Set());
    fitSupport.get(key).add(row._pass_id);
  }
  const fitBins = [...fitSupport.values()].filter(passes => passes.size >= 2).length;
  const passes = new Set(corpus.accepted.map(row => row._pass_id)).size;
  const validationPasses = new Set(corpus.validation.map(row => row._pass_id)).size;
  const gap = largestGap(covered);
  const nuisanceCells = corpus.validation.reduce((sum, row) =>
    sum + row._detection_context.nuisance.length, 0);
  const productionPasses = new Set(corpus.production_validation.map(row => row._pass_id)).size;
  const productionMissPasses = new Set(corpus.production_validation.filter(row =>
    row._baseline_outcome !== 'positive').map(row => row._pass_id)).size;
  const productionSectors = new Set(corpus.production_validation.map(row =>
    Math.floor(normalize(row.pass.truth.local_azimuth_deg) / 45) % 8)).size;
  const positiveRequirements = [
    {id: 'same_iq_passes', label: 'Same-IQ independent passes', current: passes,
      required: gates.minIndependentPasses},
    {id: 'covered_sectors', label: 'Covered azimuth sectors', current: covered.length,
      required: gates.minCoveredSectors},
    {id: 'fit_bins', label: 'Azimuth/elevation support bins', current: fitBins,
      required: gates.minFitBins}
  ].map(row => ({...row, complete: row.current >= row.required,
    fraction: Math.min(1, row.current / row.required)}));
  positiveRequirements.push({id: 'maximum_gap', label: 'Largest unsupported azimuth gap', current: gap,
    required: gates.maxUnsupportedGapDeg, comparison: 'at_most', complete: gap <= gates.maxUnsupportedGapDeg,
    fraction: gap <= gates.maxUnsupportedGapDeg ? 1 : gates.maxUnsupportedGapDeg / gap});
  const validationRequirements = [
    {id: 'validation_passes', label: 'Independent diagnostic replay passes', current: validationPasses,
      required: gates.minValidationPasses},
    {id: 'miss_passes', label: 'Diagnostic baseline miss/subthreshold passes', current: corpus.miss_passes,
      required: gates.minMissPasses},
    {id: 'validation_sectors', label: 'Diagnostic replay azimuth sectors', current: validationCovered.length,
      required: gates.minValidationSectors},
    {id: 'nuisance_cells', label: 'Diagnostic same-IQ nuisance cells', current: nuisanceCells,
      required: gates.minHoldoutNuisanceCells}
  ].map(row => ({...row, complete: row.current >= row.required,
    fraction: Math.min(1, row.current / row.required)}));
  const requirements = [...positiveRequirements, ...validationRequirements];
  const fitReady = positiveRequirements.every(row => row.complete);
  const validationReady = validationRequirements.every(row => row.complete);
  return {schema: 'vectorwarp-detection-manifold-progress-v1', enabled:
      config.process?.surveillance_fusion?.detection_manifold_automation?.enabled === true,
    config_fingerprint: corpus.fingerprint, accepted_samples: corpus.accepted.length,
    rejected_samples: corpus.invalid_samples, independent_passes: passes,
    validation_passes: validationPasses, miss_passes: corpus.miss_passes,
    covered_sectors: covered.length, validation_sectors: validationCovered.length,
    fit_bins: fitBins, holdout_nuisance_cells: nuisanceCells,
    largest_unsupported_gap_deg: gap, sectors: allCoverage,
    positive_control: {fit_ready: fitReady, requirements: positiveRequirements},
    validation: {ready: validationReady, diagnostic_only: true, requirements: validationRequirements,
      imported_replay_rows: corpus.validation.length, independent_passes: validationPasses,
      miss_passes: corpus.miss_passes},
    production_replay: {ready: false, diagnostic_only: true,
      receipt_generator_available: false, native_emitter_available: false,
      trusted_physical_cohort_required: true,
      imported_passes: productionPasses, miss_passes: productionMissPasses,
      sectors: productionSectors,
      blocker: 'The current native tool consumes caller-supplied post-CAF vectors; an authenticated raw-IQ exporter is required.'},
    qualification_ready: false,
    requirements, collection_ready: fitReady && validationReady,
    evidence_completion_fraction: requirements.length ? Math.min(...requirements.map(row => row.fraction)) : 0,
    next_actions: requirements.filter(row => !row.complete).map(row => row.comparison === 'at_most' ?
      `Reduce ${row.label.toLowerCase()} to ${row.required} degrees or less.` :
      `Collect ${row.required - row.current} more ${row.label.toLowerCase()}.`),
    limitation: 'Detected live cells, covariance-normalized replay, and the current post-CAF native grid tool are diagnostic controls only. Production acceptance requires a future authenticated exporter that derives the complete grid directly from immutable raw IQ; no path changes live configuration.'};
}

function compareTruthSelectedDiagnostic(corpus, model) {
  const byPass = new Map();
  let thresholdsConsistent = true, supportedRows = 0, totalRows = 0;
  for (const row of corpus.holdout) {
    ++totalRows;
    const steering = responseFor(model, row);
    const threshold = cfarThreshold(row._detection_context.cfar);
    if (Math.abs(row._detection_context.cfar.baseline_threshold - threshold) >
        Math.max(1e-9, threshold * 1e-6)) thresholdsConsistent = false;
    const baselineScore = row._detection_context.target.baseline_statistic;
    const targetScore = steering ? statistic(steering, row.response, row._detection_covariance) :
      baselineScore;
    if (!finite(targetScore)) continue;
    if (steering) ++supportedRows;
    const nuisance = row._detection_context.nuisance.map(item => ({
      baseline: item.baseline_detected,
      candidate: steering ?
        statistic(steering, item.response, row._detection_covariance) >= threshold :
        item.baseline_detected}));
    const result = {baseline_detected: row._detection_context.target.baseline_detected,
      candidate_detected: targetScore >= threshold,
      baseline_margin_db: 10 * Math.log10(Math.max(1e-15, baselineScore / threshold)),
      candidate_margin_db: 10 * Math.log10(Math.max(1e-15, targetScore / threshold)), nuisance,
      sector: Math.floor(normalize(row.truth.local_azimuth_deg) / 45) % 8,
      effective_trials: row._detection_context.cfar.effective_trials,
      training_cells: row._detection_context.cfar.training_cells, supported: Boolean(steering)};
    if (!byPass.has(row._pass_id)) byPass.set(row._pass_id, []);
    byPass.get(row._pass_id).push(result);
  }
  const passes = [...byPass.entries()].map(([id, rows]) => ({id,
    sector: rows[0].sector,
    baseline_detected: rows.some(row => row.baseline_detected),
    candidate_detected: rows.some(row => row.candidate_detected),
    baseline_margin_db: median(rows.map(row => row.baseline_margin_db)),
    candidate_margin_db: median(rows.map(row => row.candidate_margin_db)),
    baseline_nuisance: rows.reduce((sum, row) => sum + row.nuisance.filter(item => item.baseline).length, 0),
    candidate_nuisance: rows.reduce((sum, row) => sum + row.nuisance.filter(item => item.candidate).length, 0)}));
  const baselineDetections = passes.filter(row => row.baseline_detected).length;
  const candidateDetections = passes.filter(row => row.candidate_detected).length;
  const baselineNuisance = passes.reduce((sum, row) => sum + row.baseline_nuisance, 0);
  const candidateNuisance = passes.reduce((sum, row) => sum + row.candidate_nuisance, 0);
  return {passes: passes.length, sectors: new Set(passes.map(row => row.sector)).size,
    total_validation_rows: totalRows, supported_validation_rows: supportedRows,
    validation_support_fraction: totalRows ? supportedRows / totalRows : 0,
    baseline_recall: passes.length ? baselineDetections / passes.length : null,
    candidate_recall: passes.length ? candidateDetections / passes.length : null,
    baseline_margin_db: passes.length ? median(passes.map(row => row.baseline_margin_db)) : null,
    candidate_margin_db: passes.length ? median(passes.map(row => row.candidate_margin_db)) : null,
    target_margin_improvement_db: passes.length ? median(passes.map(row =>
      row.candidate_margin_db - row.baseline_margin_db)) : null,
    baseline_nuisance: baselineNuisance, candidate_nuisance: candidateNuisance,
    nuisance_ratio: baselineNuisance ? candidateNuisance / baselineNuisance :
      (candidateNuisance ? Infinity : 0),
    cfar_accounting: {method: 'ca-cfar-familywise-pfa-effective-trials-v1',
      baseline_thresholds_consistent: thresholdsConsistent,
      effective_trials: [...new Set(corpus.holdout.map(row => row._detection_context.cfar.effective_trials))],
      minimum_training_cells: corpus.holdout.length ? Math.min(...corpus.holdout.map(row =>
        row._detection_context.cfar.training_cells)) : 0}};
}

function compareProductionReplay(rows, candidateModelSha256, config) {
  const matching = rows.filter(row =>
    row.provenance.candidate_model_sha256 === candidateModelSha256);
  const allowlist = config.process?.surveillance_fusion?.detection_manifold_replay
    ?.trusted_native_generator_sha256;
  const trustedPolicy = Array.isArray(allowlist) && allowlist.length &&
    allowlist.every(value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value));
  // No current VectorWarp binary derives this grid directly from raw IQ.  A
  // hash allowlist cannot authenticate caller-supplied post-CAF vectors, so
  // this remains false until that exporter exists and is independently bound.
  const generatorTrusted = false;
  const passes = matching.map(row => {
    const target = row.replay.cuts.find(cut => cut.role === 'target');
    const nuisance = row.replay.cuts.filter(cut => cut.role === 'nuisance');
    const margin = arm => 10 * Math.log10(Math.max(1e-15,
      target[arm].cut_power / target[arm].threshold));
    return {id: row._pass_id, label: row._baseline_outcome,
      sector: Math.floor(normalize(row.pass.truth.local_azimuth_deg) / 45) % 8,
      baseline_detected: target.baseline.detected,
      candidate_detected: target.candidate.detected,
      baseline_margin_db: margin('baseline'), candidate_margin_db: margin('candidate'),
      baseline_nuisance: nuisance.filter(cut => cut.baseline.detected).length,
      candidate_nuisance: nuisance.filter(cut => cut.candidate.detected).length};
  });
  const baselineNuisance = passes.reduce((sum, row) => sum + row.baseline_nuisance, 0);
  const candidateNuisance = passes.reduce((sum, row) => sum + row.candidate_nuisance, 0);
  return {contract: 'vectorwarp-production-ca-cfar-full-bank-v1', diagnostic_only: true,
    candidate_model_sha256: candidateModelSha256, imported_passes: rows.length,
    matching_candidate_passes: passes.length,
    candidate_hash_mismatches: rows.length - matching.length,
    positive_passes: passes.filter(row => row.label === 'positive').length,
    miss_passes: passes.filter(row => row.label !== 'positive').length,
    sectors: new Set(passes.map(row => row.sector)).size,
    by_sector: Array.from({length: 8}, (_, sector) => ({sector,
      passes: passes.filter(row => row.sector === sector).length})),
    baseline_recall: passes.length ?
      passes.filter(row => row.baseline_detected).length / passes.length : null,
    candidate_recall: passes.length ?
      passes.filter(row => row.candidate_detected).length / passes.length : null,
    target_margin_improvement_db: passes.length ? median(passes.map(row =>
      row.candidate_margin_db - row.baseline_margin_db)) : null,
    baseline_nuisance: baselineNuisance, candidate_nuisance: candidateNuisance,
    nuisance_ratio: baselineNuisance ? candidateNuisance / baselineNuisance :
      (candidateNuisance ? Infinity : 0),
    receipt_generator_available: false, generator_trusted: generatorTrusted,
    qualification_blocker: trustedPolicy ?
      'Configured generator hashes cannot authenticate caller-supplied post-CAF vectors; a raw-IQ exporter is required.' :
      'A trusted raw-IQ full-bank exporter is not present.'};
}

function evaluateDetectionManifoldCandidate(rawSamples, config, options = {}) {
  const gates = {...DEFAULT_DETECTION_GATES, ...(options.gates || {})};
  const progress = summarizeDetectionManifoldProgress(rawSamples, config, {...options, gates});
  const corpus = buildDetectionCorpus(rawSamples, {corpusSha256: options.corpusSha256});
  const manifold = fitDetectionManifold(rawSamples, options);
  let runtimeCandidate = null, runtimeConversionError = null;
  try { runtimeCandidate = denseRuntimeCandidate(manifold); }
  catch (error) { runtimeConversionError = error.message; }
  const candidateModelSha256 = crypto.createHash('sha256').update(JSON.stringify(
    runtimeCandidate || manifold)).digest('hex');
  manifold.calibration.candidate_model_sha256 = candidateModelSha256;
  manifold.calibration.runtime_candidate_available = Boolean(runtimeCandidate);
  manifold.calibration.runtime_conversion_error = runtimeConversionError;
  const comparison = compareTruthSelectedDiagnostic(corpus, manifold), diagnosticFailures = [];
  if (!progress.collection_ready) diagnosticFailures.push(...progress.next_actions);
  if (!finite(comparison.baseline_recall) || comparison.candidate_recall < comparison.baseline_recall)
    diagnosticFailures.push('Candidate pass recall regressed on truth-selected diagnostic evidence.');
  if (!finite(comparison.target_margin_improvement_db) ||
      comparison.target_margin_improvement_db < gates.minTargetMarginImprovementDb)
    diagnosticFailures.push(`Diagnostic target margin must improve by at least ${gates.minTargetMarginImprovementDb} dB.`);
  if (!finite(comparison.nuisance_ratio) || comparison.nuisance_ratio > gates.maximumNuisanceRatio)
    diagnosticFailures.push(`Diagnostic nuisance detections exceed ${gates.maximumNuisanceRatio}x baseline.`);
  if (!finite(comparison.validation_support_fraction) ||
      comparison.validation_support_fraction < gates.minValidationSupportFraction)
    diagnosticFailures.push(`Diagnostic support covers less than ${gates.minValidationSupportFraction * 100}% of replay rows.`);
  if (comparison.cfar_accounting.minimum_training_cells < gates.minCfarTrainingCells)
    diagnosticFailures.push(`Diagnostic accounting requires at least ${gates.minCfarTrainingCells} training cells.`);
  if (!comparison.cfar_accounting.baseline_thresholds_consistent)
    diagnosticFailures.push('Diagnostic thresholds do not match the declared PFA and effective trial accounting.');
  const productionReplay = compareProductionReplay(corpus.production_validation,
    candidateModelSha256, config);
  const productionFailures = [];
  if (!productionReplay.matching_candidate_passes)
    productionFailures.push('No strict production replay receipt matches this exact candidate model.');
  if (!productionReplay.generator_trusted)
    productionFailures.push('Production replay generator is not pinned by the active config allowlist.');
  if (productionReplay.matching_candidate_passes < gates.minProductionPasses)
    productionFailures.push(`Production replay requires ${gates.minProductionPasses} matching passes.`);
  if (productionReplay.miss_passes < gates.minProductionMissPasses)
    productionFailures.push(`Production replay requires ${gates.minProductionMissPasses} independent miss/subthreshold passes.`);
  if (productionReplay.sectors < gates.minProductionSectors)
    productionFailures.push(`Production replay requires ${gates.minProductionSectors} supported azimuth sectors.`);
  if (productionReplay.by_sector.filter(row => row.passes >= gates.minProductionPassesPerSector).length <
      gates.minProductionSectors)
    productionFailures.push('Production replay does not meet the per-sector pass support gate.');
  if (!finite(productionReplay.baseline_recall) ||
      productionReplay.candidate_recall < productionReplay.baseline_recall)
    productionFailures.push('Production pass recall regressed.');
  if (!finite(productionReplay.target_margin_improvement_db) ||
      productionReplay.target_margin_improvement_db < gates.minTargetMarginImprovementDb)
    productionFailures.push(`Production target margin must improve by at least ${gates.minTargetMarginImprovementDb} dB.`);
  if (!finite(productionReplay.nuisance_ratio) ||
      productionReplay.nuisance_ratio > gates.maximumNuisanceRatio)
    productionFailures.push(`Production nuisance detections exceed ${gates.maximumNuisanceRatio}x baseline.`);
  productionReplay.gates_passed = productionFailures.length === 0;
  productionReplay.failures = productionFailures;
  const failures = [runtimeCandidate ?
    'Production qualification remains default-off; the dense bank is replay-only and cannot change live configuration.' :
    'Production qualification is disabled: the current sparse fitter is not runtime-loadable.',
    ...productionFailures];
  const evaluation = {schema: 'vectorwarp-detection-manifold-evaluation-v1',
    evaluated_at: new Date().toISOString(), qualified: false, diagnostic_only: true,
    qualification_basis: 'none-awaiting-native-full-bank-grid-export',
    promotion_allowed: false, failures, gates, progress,
    comparison: {...comparison, diagnostic_only: true,
      mode: 'truth-selected-covariance-normalized-not-production-cfar'},
    diagnostic_failures: diagnosticFailures, production_failures: productionFailures,
    production_replay: productionReplay};
  manifold.calibration.evaluation = evaluation;
  return {manifold, runtimeCandidate, evaluation, progress};
}

class DetectionManifoldAutomation {
  constructor(config, {worker = Worker, intervalMs = 15 * 60 * 1000, initialDelayMs = 30000} = {}) {
    this.config = config; this.Worker = worker; this.intervalMs = intervalMs; this.initialDelayMs = initialDelayMs;
    const settings = config.process?.surveillance_fusion?.detection_manifold_automation || {};
    this.samplePath = settings.corpus_path || config.process?.bearing?.manifold_recorder?.path || null;
    this.validationPath = settings.validation_path || null;
    this.candidatePath = settings.candidate_path || (this.samplePath ? `${this.samplePath}.detection-candidate.json` : null);
    this.receiptPath = settings.shadow_receipt_path || (this.candidatePath ?
      `${this.candidatePath}.shadow-ab-receipts.jsonl` : null);
    this.enabled = settings.enabled === true && Boolean(this.samplePath);
    this.running = false; this.timer = null; this.lastSignature = null;
    this.state = {enabled: this.enabled, state: this.enabled ? 'collecting' : 'disabled',
      qualified: false, promotion_allowed: false, candidatePath: this.candidatePath,
      validationPath: this.validationPath, receiptPath: this.receiptPath,
      error: null, progress: summarizeDetectionManifoldProgress([], config)};
  }
  status() { return {...this.state, running: this.running}; }
  start() {
    if (!this.enabled || this.timer) return;
    const schedule = delay => { this.timer = setTimeout(async () => {
      try { await this.evaluate(); } catch (_) {} schedule(this.intervalMs);
    }, delay); this.timer.unref(); };
    schedule(this.initialDelayMs);
  }
  async evaluate({force = false} = {}) {
    if (!this.enabled || this.running) return this.status();
    let stat;
    try { stat = await fs.promises.stat(this.samplePath); }
    catch (error) { if (error.code === 'ENOENT') return this.status();
      this.state = {...this.state, state: 'error', error: error.message}; return this.status(); }
    let validationStat = null;
    if (this.validationPath) try { validationStat = await fs.promises.stat(this.validationPath); }
    catch (error) { if (error.code !== 'ENOENT') {
      this.state = {...this.state, state: 'error', error: error.message}; return this.status();
    } }
    const signature = `${stat.size}:${stat.mtimeMs}:${validationStat?.size || 0}:${validationStat?.mtimeMs || 0}`;
    if (!force && signature === this.lastSignature) return this.status();
    this.running = true; this.state = {...this.state, state: 'evaluating', error: null};
    try {
      const result = await new Promise((resolve, reject) => {
        const worker = new this.Worker(path.join(__dirname, 'detection-manifold-automation-worker.js'),
          {workerData: {samplePath: this.samplePath, candidatePath: this.candidatePath,
            validationPath: this.validationPath, receiptPath: this.receiptPath, config: this.config}});
        worker.once('message', message => message.ok ? resolve(message.result) : reject(new Error(message.error)));
        worker.once('error', reject);
        worker.once('exit', code => { if (code) reject(new Error(`Detection manifold worker exited ${code}`)); });
      });
      this.lastSignature = signature;
      this.state = {...this.state, ...result, state: result.qualified ? 'validated-non-runtime' :
        result.progress?.positive_control?.fit_ready ? 'diagnostic-only-native-export-blocked' : 'collecting',
        promotion_allowed: false, error: null, lastEvaluationAt: Date.now()};
    } catch (error) {
      this.lastSignature = signature; this.state = {...this.state, state: 'collecting',
        qualified: false, promotion_allowed: false, error: error.message, lastEvaluationAt: Date.now()};
    } finally { this.running = false; }
    return this.status();
  }
}

module.exports = {DEFAULT_DETECTION_GATES, summarizeDetectionManifoldProgress,
  compareSameIq: compareTruthSelectedDiagnostic, compareTruthSelectedDiagnostic,
  compareProductionReplay, evaluateDetectionManifoldCandidate, DetectionManifoldAutomation};
