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
  const temporary = `${workerData.candidatePath}.${process.pid}.tmp`;
  await fs.promises.writeFile(temporary, JSON.stringify(manifold, null, 2) + '\n', {mode: 0o600});
  await fs.promises.rename(temporary, workerData.candidatePath);
  let runtimeCandidatePath = null;
  if (runtimeCandidate) {
    runtimeCandidatePath = `${workerData.candidatePath}.runtime.json`;
    const runtimeTemporary = `${runtimeCandidatePath}.${process.pid}.tmp`;
    await fs.promises.writeFile(runtimeTemporary, JSON.stringify(runtimeCandidate), {mode: 0o600});
    await fs.promises.rename(runtimeTemporary, runtimeCandidatePath);
  }
  parentPort.postMessage({ok: true, result: {qualified: evaluation.qualified,
    promotion_allowed: false, failures: evaluation.failures, evidence: evaluation,
    runtimeCandidatePath, progress, samples: progress.accepted_samples}});
})().catch(error => parentPort.postMessage({ok: false, error: error.message}));
