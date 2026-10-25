'use strict';

const fs = require('fs');
const crypto = require('crypto');
const {workerData, parentPort} = require('worker_threads');
const {readBounded, persistCandidate} = require('./detection-manifold-artifacts');
const {evaluateDetectionManifoldCandidate, summarizeDetectionManifoldProgress} =
  require('./detection-manifold-automation');
const {IntegrationShadowQueue} = require('./integration-shadow-queue');
const {deriveNativeShadowManifoldRow, validateNativeShadowManifoldRow,
  appendNativeShadowRow} = require('./native-shadow-manifold-evidence');
const {fitNativeShadow} = require('./native-shadow-manifold-fitter');

function importNativeShadowCorpus(config, filename) {
  const queue = new IntegrationShadowQueue(config);
  const key = queue.receiptKey();
  const keyId = crypto.createHash('sha256').update(key).digest('hex');
  const options = {resolveKey: id => id === keyId ? key : null};
  const existing = fs.existsSync(filename) ? readJsonl(filename).rows : [];
  let previous = null;
  const imports = new Set();
  for (const row of existing) { const valid = validateNativeShadowManifoldRow(row, options);
    previous = valid.row_sha256; imports.add(valid.native_export_sha256); }
  let added = 0, ignored = 0;
  for (const receipt of queue.resultRecords({includeEvidence: true}).filter(row => row.valid))
    for (const exportRow of receipt.result.truth_selected_exports || [])
      for (const frame of exportRow.multi_cpi_history || []) {
        const evidence = frame?.manifold_evidence;
        if (!evidence) continue;
        if (imports.has(evidence.export_sha256)) { ++ignored; continue; }
        const derived = deriveNativeShadowManifoldRow(evidence, {...options, signingKey: key, keyId,
          previousRowSha256: previous});
        appendNativeShadowRow(filename, derived, options);
        previous = derived.row_sha256; imports.add(evidence.export_sha256); ++added;
      }
  return {added, ignored, path: filename};
}

function readJsonl(filename) {
  const bytes = readBounded(filename);
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
  const nativePath = workerData.nativeShadowPath || `${workerData.samplePath}.native-shadow.jsonl`;
  let nativeShadow = {added: 0, ignored: 0, path: nativePath};
  try { nativeShadow = importNativeShadowCorpus(workerData.config, nativePath); }
  catch (error) { nativeShadow = {...nativeShadow, error: error.message}; }
  const corpus = fs.existsSync(workerData.samplePath) ? readJsonl(workerData.samplePath) : {bytes: Buffer.alloc(0), rows: []};
  const nativeRows = fs.existsSync(nativePath) ? readJsonl(nativePath).rows : [];
  const nativeFit = nativeRows.length ? fitNativeShadow(nativeRows) : null;
  if (!corpus.rows.length && nativeFit) {
    parentPort.postMessage({ok: true, result: {qualified: false, promotion_allowed: false,
      state: 'native-shadow-only', nativeShadow, nativeFit, failures: ['Native shadow projection only; production promotion is disabled.'],
      progress: summarizeDetectionManifoldProgress([], workerData.config), samples: 0}}); return;
  }
  const corpusSha256 = crypto.createHash('sha256').update(corpus.bytes).digest('hex');
  const samples = corpus.rows;
  if (workerData.validationPath && fs.existsSync(workerData.validationPath))
    samples.push(...readJsonl(workerData.validationPath).rows);
  const progress = summarizeDetectionManifoldProgress(samples, workerData.config, {corpusSha256});
  if (!progress.positive_control.fit_ready) {
    parentPort.postMessage({ok: true, result: {qualified: false, promotion_allowed: false,
      failures: progress.next_actions, progress, nativeShadow, samples: progress.accepted_samples}}); return;
  }
  const {manifold, runtimeCandidate, evaluation} = evaluateDetectionManifoldCandidate(samples, workerData.config,
    {corpusSha256});
  // Each model and A/B comparison is retained.  Only this small manifest is a
  // mutable pointer, and it is never read by the production processor.
  const version = manifold.candidate_version || manifold.calibration.candidate_model_sha256;
  const versionedCandidatePath = `${workerData.candidatePath}.${version}.json`;
  const candidateSha256 = persistCandidate(versionedCandidatePath, manifold);
  const manifest = {schema: 'vectorwarp-detection-manifold-shadow-manifest-v1', candidate_version: version,
    candidate_path: versionedCandidatePath, candidate_sha256: candidateSha256, evidence_key: manifold.evidence_key,
    updated_at: new Date().toISOString(), production_promotion: 'hard-disabled'};
  const temporary = `${workerData.candidatePath}.${process.pid}.tmp`;
  await fs.promises.writeFile(temporary, JSON.stringify(manifest, null, 2) + '\n', {mode: 0o600});
  const receiptPath = workerData.receiptPath || `${workerData.candidatePath}.shadow-ab-receipts.jsonl`;
  await fs.promises.appendFile(receiptPath, JSON.stringify({schema: 'vectorwarp-detection-manifold-shadow-ab-receipt-v1',
    at: manifest.updated_at, candidate_version: version, candidate_path: versionedCandidatePath,
    candidate_sha256: candidateSha256,
    evidence_key: manifold.evidence_key, baseline: 'imported-same-iq-baseline-statistics', candidate: 'measured-2d-shrunk',
    evaluation, production_promotion: 'hard-disabled'}) + '\n', {mode: 0o600});
  await fs.promises.rename(temporary, workerData.candidatePath);
  parentPort.postMessage({ok: true, result: {qualified: evaluation.qualified,
    promotion_allowed: false, failures: evaluation.failures, evidence: evaluation,
    candidateVersion: version, versionedCandidatePath, receiptPath,
    runtimeCandidatePath: null, progress, nativeShadow, samples: progress.accepted_samples}});
})().catch(error => parentPort.postMessage({ok: false, error: error.message}));
