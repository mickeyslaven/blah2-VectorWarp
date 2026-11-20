#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const {fitManifold} = require('../api/manifold-fitter');
const {createRequire} = require('module');
const yaml = createRequire(path.join(__dirname, '../api/package.json'))('js-yaml');
const {evaluateManifoldCandidate} = require('./manifold-evaluator');
const {LIMITS, readBoundedFile, readBoundedJsonl, writeBoundedCandidate} = require('./manifold-fit-limits');

function usage() {
  return 'Usage: fit-bearing-manifold --input samples.jsonl [--input rotated.jsonl.1] --output manifold.json [--config config.yml] [--beams 60] [--min-samples 20] [--min-coverage 0.5] [--max-gap-deg 90]';
}

function argumentsFrom(argv) {
  const options = {input: [], minSamples: 20, minCoverage: .5, maxGapDeg: 90};
  for (let index = 0; index < argv.length; ++index) {
    const name = argv[index], value = argv[++index];
    if (value === undefined) throw new Error(usage());
    if (name === '--input') options.input.push(value);
    else if (name === '--output') options.output = value;
    else if (name === '--config') options.config = value;
    else if (name === '--beams') options.beams = Number(value);
    else if (name === '--min-samples') options.minSamples = Number(value);
    else if (name === '--min-coverage') options.minCoverage = Number(value);
    else if (name === '--max-gap-deg') options.maxGapDeg = Number(value);
    else throw new Error(`Unknown option ${name}\n${usage()}`);
  }
  if (!options.input.length || !options.output || !path.isAbsolute(options.output))
    throw new Error(`${usage()}\nThe output path must be absolute.`);
  return options;
}

async function main() {
  const options = argumentsFrom(process.argv.slice(2));
  const samples = [];
  let inputBytes = 0;
  for (const filename of options.input) {
    const corpus = readBoundedJsonl(filename);
    inputBytes += corpus.bytes.length;
    if (inputBytes > LIMITS.input_bytes || samples.length + corpus.rows.length > LIMITS.input_rows)
      throw new Error('Combined inputs exceed the 64 MiB / 20000-row limit');
    samples.push(...corpus.rows);
  }
  let config;
  if (options.config) {
    config = yaml.load(readBoundedFile(options.config, LIMITS.config_bytes,
      'Manifold configuration (256 KiB)').toString('utf8'), {schema: yaml.JSON_SCHEMA});
  }
  const result = config ? evaluateManifoldCandidate(samples, config, options).manifold :
    fitManifold(samples, options);
  await fs.promises.mkdir(path.dirname(options.output), {recursive: true});
  await writeBoundedCandidate(options.output, result);
  process.stdout.write(`Wrote ${options.output}: ${result.calibration.input_samples} samples, ` +
    `${result.calibration.observed_bins}/${result.calibration.beams} observed bins, ` +
    `${result.calibration.interpolated_bins} interpolated.\n`);
}

main().catch(error => { process.stderr.write(`fit-bearing-manifold: ${error.message}\n`); process.exitCode = 1; });
