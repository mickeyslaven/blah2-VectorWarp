'use strict';

const fs = require('fs');
const {workerData, parentPort} = require('worker_threads');
const {readSamples} = require('./bearing-diagnostic');
const {evaluateManifoldCandidate, summarizeManifoldProgress} = require('./manifold-automation');

async function atomicWrite(filename, value) {
  const temporary = `${filename}.${process.pid}.tmp`;
  await fs.promises.writeFile(temporary, JSON.stringify(value, null, 2) + '\n', {mode: 0o600});
  await fs.promises.rename(temporary, filename);
}

(async () => {
  const samples = readSamples(workerData.samplePath);
  const progress = summarizeManifoldProgress(samples, workerData.config);
  if (!progress.collection_ready) {
    // Insufficient evidence is normal while collecting. Preserve a complete
    // progress snapshot instead of presenting it as a worker failure.
    parentPort.postMessage({ok: true, result: {qualified: false,
      failures: progress.next_actions, evidence: null, progress,
      samples: samples.length, fingerprint: progress.config_fingerprint}});
    return;
  }
  const {manifold, evaluation} = evaluateManifoldCandidate(samples, workerData.config);
  await atomicWrite(workerData.candidatePath, manifold);
  if (evaluation.qualified) await atomicWrite(workerData.qualifiedPath, manifold);
  parentPort.postMessage({ok: true, result: {qualified: evaluation.qualified,
    failures: evaluation.failures, evidence: evaluation, progress,
    samples: samples.length, fingerprint: manifold.calibration.config_fingerprint}});
})().catch(error => parentPort.postMessage({ok: false, error: error.message}));
