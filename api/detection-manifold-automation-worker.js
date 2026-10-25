'use strict';

const fs = require('fs');
const crypto = require('crypto');
const {workerData, parentPort} = require('worker_threads');
const {evaluateDetectionManifoldCandidate, summarizeDetectionManifoldProgress} =
  require('./detection-manifold-automation');

function readJsonl(filename) {
  const bytes = fs.readFileSync(filename);
  if (bytes.length > 64 * 1024 * 1024)
    throw new Error('Detection manifold corpus exceeds the read limit');
  const lines = bytes.toString('utf8').split(/\r?\n/).filter(Boolean);
  if (lines.length > 20000) throw new Error('Detection manifold corpus exceeds the row limit');
  return {bytes, rows: lines.map((line, index) => {
    try { return JSON.parse(line); }
    catch (error) { throw new Error(`Invalid detection corpus line ${index + 1}: ${error.message}`); }
  })};
}

(async () => {
  const corpus = readJsonl(workerData.samplePath);
  const corpusSha256 = crypto.createHash('sha256').update(corpus.bytes).digest('hex');
  const samples = corpus.rows;
  if (workerData.validationPath && fs.existsSync(workerData.validationPath))
    samples.push(...readJsonl(workerData.validationPath).rows);
  const progress = summarizeDetectionManifoldProgress(samples, workerData.config, {corpusSha256});
  if (!progress.positive_control.fit_ready) {
    parentPort.postMessage({ok: true, result: {qualified: false, promotion_allowed: false,
      failures: progress.next_actions, progress, samples: progress.accepted_samples}}); return;
  }
  const {manifold, runtimeCandidate, evaluation} = evaluateDetectionManifoldCandidate(samples, workerData.config,
    {corpusSha256});
  // Each model and A/B comparison is retained.  Only this small manifest is a
  // mutable pointer, and it is never read by the production processor.
  const version = manifold.candidate_version || manifold.calibration.candidate_model_sha256;
  const versionedCandidatePath = `${workerData.candidatePath}.${version}.json`;
  try { await fs.promises.access(versionedCandidatePath); }
  catch (_) { await fs.promises.writeFile(versionedCandidatePath, JSON.stringify(manifold, null, 2) + '\n', {mode: 0o600}); }
  const manifest = {schema: 'vectorwarp-detection-manifold-shadow-manifest-v1', candidate_version: version,
    candidate_path: versionedCandidatePath, evidence_key: manifold.evidence_key,
    updated_at: new Date().toISOString(), production_promotion: 'hard-disabled'};
  const temporary = `${workerData.candidatePath}.${process.pid}.tmp`;
  await fs.promises.writeFile(temporary, JSON.stringify(manifest, null, 2) + '\n', {mode: 0o600});
  await fs.promises.rename(temporary, workerData.candidatePath);
  const receiptPath = workerData.receiptPath || `${workerData.candidatePath}.shadow-ab-receipts.jsonl`;
  await fs.promises.appendFile(receiptPath, JSON.stringify({schema: 'vectorwarp-detection-manifold-shadow-ab-receipt-v1',
    at: manifest.updated_at, candidate_version: version, candidate_path: versionedCandidatePath,
    evidence_key: manifold.evidence_key, baseline: 'ideal-manifold', candidate: 'measured-2d-shrunk',
    evaluation, production_promotion: 'hard-disabled'}) + '\n', {mode: 0o600});
  parentPort.postMessage({ok: true, result: {qualified: evaluation.qualified,
    promotion_allowed: false, failures: evaluation.failures, evidence: evaluation,
    candidateVersion: version, versionedCandidatePath, receiptPath,
    runtimeCandidatePath: null, progress, samples: progress.accepted_samples}});
})().catch(error => parentPort.postMessage({ok: false, error: error.message}));
