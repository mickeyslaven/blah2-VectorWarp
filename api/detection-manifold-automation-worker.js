'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {workerData, parentPort} = require('worker_threads');
const {readBounded, validateArtifactDestinations, publishShadow} = require('./detection-manifold-artifacts');
const {evaluateDetectionManifoldCandidate, summarizeDetectionManifoldProgress} =
  require('./detection-manifold-automation');
const {IntegrationShadowQueue} = require('./integration-shadow-queue');
const {deriveNativeShadowManifoldRow, validateNativeShadowManifoldRow,
  appendNativeShadowRows} = require('./native-shadow-manifold-evidence');
const {fitNativeShadow} = require('./native-shadow-manifold-fitter');

function importNativeShadowCorpus(config, filename, candidatePath) {
  const queue = new IntegrationShadowQueue(config);
  const key = queue.receiptKey();
  const keyId = crypto.createHash('sha256').update(key).digest('hex');
  const options = {resolveKey: id => id === keyId ? key : null};
  const initial = fs.existsSync(filename) ? readJsonl(filename) : {bytes: Buffer.alloc(0), rows: []};
  const existing = initial.rows;
  let previous = null;
  const imports = new Set();
  for (const row of existing) { const valid = validateNativeShadowManifoldRow(row, options);
    if (valid.previous_row_sha256 !== previous) throw new Error('Native shadow ledger chain is discontinuous');
    if (imports.has(valid.native_export_sha256)) throw new Error('Duplicate native export in shadow ledger');
    previous = valid.row_sha256; imports.add(valid.native_export_sha256); }
  if (fs.existsSync(candidatePath)) {
    const anchor = JSON.parse(readBounded(candidatePath, 1024 * 1024)).native_ledger;
    if (anchor?.path === filename && (!Number.isSafeInteger(anchor.rows) || anchor.rows < 1 ||
        existing.length < anchor.rows || existing[anchor.rows - 1]?.row_sha256 !== anchor.tail_sha256))
      throw new Error('Native shadow ledger lost its previously published authenticated prefix');
  }
  let added = 0, ignored = 0, rejected = 0, unavailable = 0;
  const pending = [];
  let pendingBytes = initial.bytes.length;
  for (const receipt of queue.resultRecordIterator({includeEvidence: true, includeArchived: true})) {
    if (!receipt.valid) { ++rejected; continue; }
    for (const exportRow of receipt.result.truth_selected_exports || [])
      for (const frame of exportRow.multi_cpi_history || []) {
        const evidence = frame?.manifold_evidence;
        if (!evidence) { ++unavailable; continue; }
        if (evidence.job_binding_sha256 !== receipt.result.job_binding_sha256 ||
            !evidence.source_snapshots?.some(snapshot => snapshot.sha256 === frame.native_snapshot?.sha256) ||
            JSON.stringify(evidence.target_cut?.physical_channels) !== JSON.stringify(frame.target_cut?.physical_channels))
          throw new Error('Native manifold evidence differs from its authenticated receipt/frame binding');
        if (imports.has(evidence.export_sha256)) { ++ignored; continue; }
        const derived = deriveNativeShadowManifoldRow(evidence, {...options, signingKey: key, keyId,
          previousRowSha256: previous});
        pending.push(derived);
        pendingBytes += Buffer.byteLength(JSON.stringify(derived)) + 1;
        if (pendingBytes > 64 * 1024 * 1024) throw new Error('Native shadow import exceeds its byte bound');
        if (pending.length + existing.length > 20000) throw new Error('Native shadow import exceeds its row bound');
        previous = derived.row_sha256; imports.add(evidence.export_sha256); ++added;
      }
  }
  if (pending.length) {
    fs.mkdirSync(path.dirname(filename), {recursive: true, mode: 0o700});
    appendNativeShadowRows(filename, pending, options);
  }
  return {summary: {added, ignored, rejected, unavailable_frames: unavailable, path: filename}, options,
    anchor: previous ? {path: filename, rows: existing.length + added, tail_sha256: previous} : null};
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
  const paths = {...workerData, nativeShadowPath: nativePath,
    receiptPath: workerData.receiptPath || `${workerData.candidatePath}.shadow-ab-receipts.jsonl`};
  validateArtifactDestinations(workerData.config, paths);
  let nativeShadow = {added: 0, ignored: 0, path: nativePath};
  let nativeFit = null;
  if (workerData.config.process?.detection?.integration_shadow?.enabled === true || fs.existsSync(nativePath)) {
    const imported = importNativeShadowCorpus(workerData.config, nativePath, workerData.candidatePath);
    nativeShadow = imported.summary;
    paths.nativeLedgerAnchor = imported.anchor;
    const nativeRows = fs.existsSync(nativePath) ? readJsonl(nativePath).rows : [];
    // Authentication/semantic/chain failures are terminal for this evaluation;
    // never reread an untrusted ledger after swallowing an import exception.
    nativeFit = nativeRows.length ? fitNativeShadow(nativeRows, {...imported.options,
      cpiSeconds: workerData.config.process?.data?.cpi}) : null;
  }
  if (nativeFit) {
    const published = publishShadow(workerData.config, paths, nativeFit.model, nativeFit.evaluation,
      nativeFit.model.baseline, 'measured-native-2d-shrunk-projection-only');
    parentPort.postMessage({ok: true, result: {qualified: false, promotion_allowed: false,
      state: 'native-shadow-projection-only', nativeShadow, ...published,
      nativeFit: {accepted_samples: nativeFit.accepted_samples,
        excluded_prior_epoch_samples: nativeFit.excluded_prior_epoch_samples,
        excluded_other_configuration_samples: nativeFit.excluded_other_configuration_samples,
        training_passes: nativeFit.model.train_passes.length,
        holdout_passes: nativeFit.evaluation.holdout_passes.length,
        supported_cells: nativeFit.supported_cells,
        supported_holdout_rows: nativeFit.evaluation.diagnostics.filter(row => row.supported).length,
        qualification: 'disabled-shadow-projection-only'},
      evidence: nativeFit.evaluation, runtimeCandidatePath: null,
      failures: ['Native shadow projection only; independent physical qualification and production promotion remain disabled.'],
      progress: summarizeDetectionManifoldProgress([], workerData.config), samples: nativeFit.accepted_samples}}); return;
  }
  const corpus = workerData.samplePath && fs.existsSync(workerData.samplePath) ? readJsonl(workerData.samplePath) : {bytes: Buffer.alloc(0), rows: []};
  const corpusSha256 = crypto.createHash('sha256').update(corpus.bytes).digest('hex');
  const samples = corpus.rows;
  if (workerData.validationPath && fs.existsSync(workerData.validationPath))
    samples.push(...readJsonl(workerData.validationPath).rows);
  const progress = summarizeDetectionManifoldProgress(samples, workerData.config, {corpusSha256});
  if (!progress.positive_control.fit_ready) {
    parentPort.postMessage({ok: true, result: {qualified: false, promotion_allowed: false,
      failures: progress.next_actions, progress, nativeShadow, samples: progress.accepted_samples}}); return;
  }
  const {manifold, evaluation} = evaluateDetectionManifoldCandidate(samples, workerData.config,
    {corpusSha256});
  // Each model and A/B comparison is retained.  Only this small manifest is a
  // mutable pointer, and it is never read by the production processor.
  const published = publishShadow(workerData.config, paths, manifold, evaluation,
    'imported-same-iq-baseline-statistics', 'measured-2d-shrunk');
  parentPort.postMessage({ok: true, result: {qualified: evaluation.qualified,
    promotion_allowed: false, failures: evaluation.failures, evidence: evaluation,
    ...published,
    runtimeCandidatePath: null, progress, nativeShadow, samples: progress.accepted_samples}});
})().catch(error => parentPort.postMessage({ok: false, error: error.message}));
