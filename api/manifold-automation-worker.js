'use strict';

const fs = require('fs');
const {workerData, parentPort} = require('worker_threads');
const {readSamples} = require('./bearing-diagnostic');
const {evaluateManifoldCandidate} = require('./manifold-automation');

async function atomicWrite(filename, value) {
  const temporary = `${filename}.${process.pid}.tmp`;
  await fs.promises.writeFile(temporary, JSON.stringify(value, null, 2) + '\n', {mode: 0o600});
  await fs.promises.rename(temporary, filename);
}

(async () => {
  const samples = readSamples(workerData.samplePath);
  const {manifold, evaluation} = evaluateManifoldCandidate(samples, workerData.config);
  await atomicWrite(workerData.candidatePath, manifold);
  if (evaluation.qualified) await atomicWrite(workerData.qualifiedPath, manifold);
  parentPort.postMessage({ok: true, result: {qualified: evaluation.qualified,
    failures: evaluation.failures, evidence: evaluation,
    samples: samples.length, fingerprint: manifold.calibration.config_fingerprint}});
})().catch(error => parentPort.postMessage({ok: false, error: error.message}));
