#!/usr/bin/env node
'use strict';

// Explicit offline derivation of a measured detection bank. A fit cannot edit
// live configuration, activate a model, or replace an existing output artifact.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {createRequire} = require('module');
const {Worker, isMainThread, workerData, parentPort} = require('worker_threads');
const yaml = createRequire(path.join(__dirname, '../api/package.json'))('js-yaml');
const {LIMITS, readBoundedFile, readBoundedJsonl, writeBoundedCandidate, runBoundedWorker} =
  require('./manifold-fit-limits');
const {evaluateDetectionManifoldCandidate} = require('./detection-manifold-evaluator');

function argumentsFrom(argv) {
  const options = {};
  for (let index = 0; index < argv.length; ++index) {
    const name = argv[index];
    if (name === '--enable-offline-experiment') { options.enabled = true; continue; }
    if (!['--input', '--validation', '--config', '--output', '--runtime-output'].includes(name) ||
        !argv[index + 1] || argv[index + 1].startsWith('--'))
      throw new Error(`Unknown or incomplete option ${name}`);
    const key = name.slice(2);
    if (options[key] !== undefined) throw new Error(`Duplicate option ${name}`);
    options[key] = argv[++index];
  }
  if (!options.enabled || !options.input || !options.config || !options.output)
    throw new Error('Usage: fit-detection-manifold --enable-offline-experiment --input samples.jsonl --config config.yml --output candidate.json [--validation replay.jsonl] [--runtime-output dense-bank.json]');
  const outputs = [options.output, options['runtime-output']].filter(Boolean);
  if (outputs.some(filename => !path.isAbsolute(filename)))
    throw new Error('Output paths must be absolute');
  if (new Set(outputs.map(filename => path.resolve(filename))).size !== outputs.length)
    throw new Error('Candidate and dense-bank outputs must be distinct');
  return options;
}

function fit(options) {
  const configBytes = readBoundedFile(options.config, LIMITS.config_bytes,
    'Manifold configuration (256 KiB)');
  const config = yaml.load(configBytes.toString('utf8'), {schema: yaml.JSON_SCHEMA});
  if (!config || typeof config !== 'object' || Array.isArray(config))
    throw new Error('Manifold configuration must be an object');
  const corpus = readBoundedJsonl(options.input);
  const corpusSha256 = crypto.createHash('sha256').update(corpus.bytes).digest('hex');
  if (options.validation) {
    const validation = readBoundedJsonl(options.validation);
    if (corpus.bytes.length + validation.bytes.length > LIMITS.input_bytes ||
        corpus.rows.length + validation.rows.length > LIMITS.input_rows)
      throw new Error('Combined inputs exceed the 64 MiB / 20000-row limit');
    corpus.rows.push(...validation.rows);
  }
  return evaluateDetectionManifoldCandidate(corpus.rows, config, {corpusSha256});
}

async function main(argv) {
  const options = argumentsFrom(argv);
  for (const filename of [options.output, options['runtime-output']].filter(Boolean)) {
    try { await fs.promises.lstat(filename); throw new Error(`Output already exists: ${filename}`); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  const result = await runBoundedWorker(Worker, __filename, options);
  if (options['runtime-output'] && !result.runtimeCandidate)
    throw new Error('Incomplete beam/elevation support: no dense bank is available');
  // Validate both output sizes before publishing either artifact.
  const outputs = [[options.output, result.manifold],
    ...(options['runtime-output'] ? [[options['runtime-output'], result.runtimeCandidate]] : [])];
  for (const [, value] of outputs)
    if (Buffer.byteLength(JSON.stringify(value, null, 2) + '\n') > LIMITS.output_bytes)
      throw new Error('Manifold experiment candidate exceeds 2 MiB');
  for (const [filename, value] of outputs) await writeBoundedCandidate(filename, value);
  process.stdout.write(JSON.stringify({candidate: options.output,
    dense_bank: options['runtime-output'] || null, qualified: false,
    failures: result.evaluation.failures}) + '\n');
}

if (!isMainThread) {
  try { parentPort.postMessage({ok: true, result: fit(workerData)}); }
  catch (error) { parentPort.postMessage({ok: false, error: error.message}); }
} else if (require.main === module) {
  main(process.argv.slice(2)).catch(error => {
    process.stderr.write(`fit-detection-manifold: ${error.message}\n`); process.exitCode = 1;
  });
}

module.exports = {argumentsFrom};
